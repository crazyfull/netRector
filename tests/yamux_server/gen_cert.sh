#!/bin/sh
# Create a self-signed cert.pem / key.pem for the yamux test server.
# CN=localhost, SAN: localhost + 127.0.0.1, valid for 10 years.
# Usage: ./gen_cert.sh [output-dir]   (default: the directory of this script)
# Uses a config file instead of -addext so it works with OpenSSL and LibreSSL.
set -eu

OUT="${1:-$(cd "$(dirname "$0")" && pwd)}"
CFG="$(mktemp)"
trap 'rm -f "$CFG"' EXIT

cat > "$CFG" <<'EOF'
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no

[dn]
CN = localhost

[v3]
subjectAltName = DNS:localhost, IP:127.0.0.1
basicConstraints = critical, CA:TRUE
keyUsage = critical, digitalSignature, keyEncipherment, keyCertSign
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
EOF

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
    -config "$CFG" -keyout "$OUT/key.pem" -out "$OUT/cert.pem"
chmod 600 "$OUT/key.pem"
echo "wrote $OUT/cert.pem and $OUT/key.pem"
