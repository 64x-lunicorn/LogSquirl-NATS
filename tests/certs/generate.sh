#!/usr/bin/env bash
# Regenerates the TEST-ONLY certificates in this directory, valid for 100 years:
#
#   ca-cert.pem                      the test CA
#   server-cert.pem, server-key.pem  for localhost / 127.0.0.1, signed by it
#   client-cert.pem, client-key.pem  a client certificate signed by it
#   other-ca-cert.pem                another CA, which the server does not trust
#   other-client-cert.pem, other-client-key.pem
#                                    a client certificate signed by that one
#
# They are committed rather than made at test time so the TLS test needs no
# openssl program (none on the Windows runners) and is the same everywhere.
# The keys protect nothing: never use them outside these tests.
#
# Usage: tests/certs/generate.sh   (needs openssl 1.1.1 or later)
set -euo pipefail
cd "$(dirname "$0")"

days=36500
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# ca <name> <common name>: <name>-cert.pem, a self-signed CA; its key,
# <name>-key.pem, only in the work directory.
ca() {
  local name=$1 cn=$2
  openssl ecparam -name prime256v1 -genkey -noout -out "$work/$name-key.pem"
  openssl req -x509 -new -key "$work/$name-key.pem" -sha256 -days "$days" \
    -subj "/CN=$cn" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$name-cert.pem"
}

# sign <name> <ca-name> <extendedKeyUsage> [subjectAltName] [common name]:
# <name>-key.pem and <name>-cert.pem, signed by <ca-name>-cert.pem; the
# common name is "logsquirl-nats test <name>" unless given.
sign() {
  local name=$1 ca=$2 usage=$3 san=${4:-} cn=${5:-"logsquirl-nats test $1"}
  openssl ecparam -name prime256v1 -genkey -noout -out "$work/$name-key-ec.pem"
  openssl pkcs8 -topk8 -nocrypt -in "$work/$name-key-ec.pem" -out "$name-key.pem"
  openssl req -new -key "$name-key.pem" -subj "/CN=$cn" -out "$work/$name.csr"
  {
    echo "basicConstraints=critical,CA:FALSE"
    echo "keyUsage=critical,digitalSignature"
    echo "extendedKeyUsage=$usage"
    if [ -n "$san" ]; then echo "subjectAltName=$san"; fi
  } > "$work/$name.ext"
  openssl x509 -req -in "$work/$name.csr" -CA "$ca-cert.pem" -CAkey "$work/$ca-key.pem" \
    -CAcreateserial -CAserial "$work/$ca.srl" -sha256 -days "$days" \
    -extfile "$work/$name.ext" -out "$name-cert.pem"
}

ca ca "logsquirl-nats test CA"
sign server ca serverAuth "DNS:localhost,IP:127.0.0.1,IP:::1" localhost
sign client ca clientAuth

ca other-ca "logsquirl-nats other test CA"
sign other-client other-ca clientAuth

# The CA keys are thrown away: nothing else can ever be signed with them.
openssl verify -CAfile ca-cert.pem server-cert.pem client-cert.pem
openssl verify -CAfile other-ca-cert.pem other-client-cert.pem
