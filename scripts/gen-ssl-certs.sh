#!/usr/bin/env bash
# Generates a local development CA and a server certificate signed by it.
#
#   ssl/ca.key, ssl/ca.crt         - development CA (trust ca.crt in clients, e.g. curl --cacert ssl/ca.crt)
#   ssl/server.key, ssl/server.crt - server key/cert for localhost / 127.0.0.1 / ::1
#
# Usage: scripts/gen-ssl-certs.sh [output_dir]   (default: ./ssl)
# Override the openssl binary with OPENSSL=/path/to/openssl.
set -euo pipefail

OUT_DIR="${1:-ssl}"

if [[ -z "${OPENSSL:-}" ]]; then
    if command -v brew >/dev/null 2>&1 && [[ -x "$(brew --prefix openssl@3 2>/dev/null)/bin/openssl" ]]; then
        OPENSSL="$(brew --prefix openssl@3)/bin/openssl"
    else
        OPENSSL=openssl
    fi
fi

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

DAYS=825 # max validity accepted by Apple platforms for TLS server certs

# Development CA
"$OPENSSL" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ca.key
"$OPENSSL" req -x509 -new -key ca.key -sha256 -days 3650 \
    -subj "/CN=rooster dev CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out ca.crt

# Server key + CSR
"$OPENSSL" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out server.key
"$OPENSSL" req -new -key server.key -subj "/CN=localhost" -out server.csr

cat > server.ext <<EXT
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1
EXT

"$OPENSSL" x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS" -sha256 -extfile server.ext -out server.crt

rm -f server.csr server.ext ca.srl
chmod 600 ca.key server.key

echo "Generated TLS files in $(pwd):"
"$OPENSSL" x509 -in server.crt -noout -subject -issuer -enddate -ext subjectAltName
