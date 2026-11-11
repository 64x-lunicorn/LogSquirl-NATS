#!/usr/bin/env bash
# Regenerates the TEST-ONLY certificates in this directory: a CA and a server
# certificate for localhost / 127.0.0.1 signed by it, valid for 100 years.
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

openssl ecparam -name prime256v1 -genkey -noout -out "$work/ca-key.pem"
openssl req -x509 -new -key "$work/ca-key.pem" -sha256 -days "$days" \
  -subj "/CN=logsquirl-nats test CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out ca-cert.pem

openssl ecparam -name prime256v1 -genkey -noout -out "$work/server-key-ec.pem"
openssl pkcs8 -topk8 -nocrypt -in "$work/server-key-ec.pem" -out server-key.pem
openssl req -new -key server-key.pem -subj "/CN=localhost" -out "$work/server.csr"
cat > "$work/server.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1
EOF
openssl x509 -req -in "$work/server.csr" -CA ca-cert.pem -CAkey "$work/ca-key.pem" \
  -CAcreateserial -CAserial "$work/ca.srl" -sha256 -days "$days" \
  -extfile "$work/server.ext" -out server-cert.pem

# The CA key is thrown away: nothing else can ever be signed with it.
openssl verify -CAfile ca-cert.pem server-cert.pem
