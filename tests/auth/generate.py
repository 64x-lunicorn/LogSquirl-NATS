#!/usr/bin/env python3
"""Regenerates the TEST-ONLY NATS credentials in this directory.

  operator.conf   a nats-server configuration in operator (JWT) mode: the
                  operator's JWT and, preloaded in a memory resolver, the
                  JWT of the account "tests"
  user.creds      the JWT of the user "tests" of that account, with its
                  NKey seed: what natsOptions_SetUserCredentialsFromFiles reads
  other.creds     a user of an account the server does not know (a wrong
                  .creds file)
  nkey.seed       the NKey seed of a user for nkey authentication
  nkey.pub        its public key, for the server's configuration
  other.seed      another NKey seed the server does not know

They are committed rather than made at test time, so the tests need neither
nsc nor any other tool, and are the same everywhere.  The JWTs never expire.
The keys protect nothing: never use them outside these tests.

Instead of nsc, this script signs the JWTs itself, with ed25519 keys made by
openssl; nats-server checks every signature when the tests run, so a
mistake here fails them.

Usage: python3 tests/auth/generate.py   (needs Python 3.8+ and openssl 1.1.1
or later on the PATH)
"""

import base64
import hashlib
import json
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
IAT = 1798934400  # 2027-01-01, fixed so that only the keys change

# NKey prefix bytes (nkeys/strkey.go): the first base32 character of the key.
PREFIX_SEED = 18 << 3      # S
PREFIX_OPERATOR = 14 << 3  # O
PREFIX_ACCOUNT = 0         # A
PREFIX_USER = 20 << 3      # U


def crc16(data):
    """CRC-16/XMODEM, as nkeys appends to every key."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def encode(prefix_bytes, raw):
    data = bytes(prefix_bytes) + raw
    data += crc16(data).to_bytes(2, "little")
    return base64.b32encode(data).decode().rstrip("=")


def b64url(data):
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


class KeyPair:
    """An ed25519 key made by openssl, with its NKey encodings."""

    def __init__(self, work, prefix):
        self.prefix = prefix
        self.pem = os.path.join(work, "key%d.pem" % id(self))
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", self.pem],
                       check=True)
        private = subprocess.run(["openssl", "pkey", "-in", self.pem, "-outform", "DER"],
                                 check=True, capture_output=True).stdout
        public = subprocess.run(["openssl", "pkey", "-in", self.pem, "-pubout", "-outform",
                                 "DER"], check=True, capture_output=True).stdout
        self.raw_seed = private[-32:]
        self.raw_public = public[-32:]

    @property
    def public(self):
        return encode([self.prefix], self.raw_public)

    @property
    def seed(self):
        return encode([PREFIX_SEED | (self.prefix >> 5), (self.prefix & 31) << 3],
                      self.raw_seed)

    def sign(self, data):
        work = os.path.dirname(self.pem)
        message = os.path.join(work, "message")
        with open(message, "wb") as f:
            f.write(data)
        return subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", self.pem, "-rawin",
                               "-in", message], check=True, capture_output=True).stdout


def jwt(issuer, subject, name, nats):
    claims = {"iat": IAT, "iss": issuer.public, "name": name, "sub": subject.public,
              "nats": nats}
    # jti: a hash of the claims without it, as nats-io/jwt does.
    claims["jti"] = base64.b32encode(
        hashlib.sha256(json.dumps(claims, sort_keys=True).encode()).digest()
    ).decode().rstrip("=")
    header = b64url(json.dumps({"typ": "JWT", "alg": "ed25519-nkey"}).encode())
    body = b64url(json.dumps(claims, sort_keys=True).encode())
    signing_input = (header + "." + body).encode()
    return header + "." + body + "." + b64url(issuer.sign(signing_input))


ACCOUNT_LIMITS = {"subs": -1, "data": -1, "payload": -1, "imports": -1, "exports": -1,
                  "wildcards": True, "conn": -1, "leaf": -1}
USER_LIMITS = {"pub": {}, "sub": {}, "subs": -1, "data": -1, "payload": -1}


def creds(user_jwt, user):
    return ("-----BEGIN NATS USER JWT-----\n%s\n------END NATS USER JWT------\n\n"
            "************************* IMPORTANT *************************\n"
            "NKEY Seed printed below can be used to sign and prove identity.\n"
            "NKEYs are sensitive and should be treated as secrets.\n\n"
            "-----BEGIN USER NKEY SEED-----\n%s\n------END USER NKEY SEED------\n\n"
            "*************************************************************\n"
            % (user_jwt, user.seed))


def account_user(work, operator, account_name):
    """An account signed by @operator and a user of it: (account, its JWT, creds)."""
    account = KeyPair(work, PREFIX_ACCOUNT)
    account_jwt = jwt(operator, account, account_name,
                      {"type": "account", "version": 2, "limits": ACCOUNT_LIMITS})
    user = KeyPair(work, PREFIX_USER)
    user_jwt = jwt(account, user, "tests", dict(USER_LIMITS, type="user", version=2))
    return account, account_jwt, creds(user_jwt, user)


def write(name, text):
    with open(os.path.join(HERE, name), "w", newline="\n") as f:
        f.write(text)


def main():
    with tempfile.TemporaryDirectory() as work:
        operator = KeyPair(work, PREFIX_OPERATOR)
        operator_jwt = jwt(operator, operator, "logsquirl-nats tests",
                           {"type": "operator", "version": 2})
        account, account_jwt, user_creds = account_user(work, operator, "tests")
        system, system_jwt, _ = account_user(work, operator, "SYS")
        _, _, other_creds = account_user(work, KeyPair(work, PREFIX_OPERATOR), "other")

        write("operator.conf",
              "# TEST-ONLY: generated by generate.py, see there.\n"
              "operator: %s\n"
              "system_account: %s\n"
              "resolver: MEMORY\n"
              "resolver_preload: {\n"
              "  %s: %s\n"
              "  %s: %s\n"
              "}\n" % (operator_jwt, system.public, account.public, account_jwt,
                       system.public, system_jwt))
        write("user.creds", user_creds)
        write("other.creds", other_creds)

        nkey_user = KeyPair(work, PREFIX_USER)
        write("nkey.seed", nkey_user.seed + "\n")
        write("nkey.pub", nkey_user.public + "\n")
        write("other.seed", KeyPair(work, PREFIX_USER).seed + "\n")


if __name__ == "__main__":
    main()
