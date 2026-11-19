#!/usr/bin/env bash
# Adds the record-protocol-version API used by TCPSocket::enableTLS(ctx, bypassMode, ...) to a
# BoringSSL source tree. It must be applied BEFORE BoringSSL is built. Safe to run more than once.
#
#   scripts/patch_boringssl.sh /path/to/boringssl
#
# Changes:
#   include/openssl/ssl.h     OPENSSL_EXPORT int SSL_set_record_protocol_version(SSL *ssl, uint16_t version);
#                             #define TLS_CL_VERSION 0x0306
#   ssl/ssl_versions.cc       int SSL_set_record_protocol_version(SSL *ssl, uint16_t version) { ... }
set -euo pipefail

SRC="${1:?usage: patch_boringssl.sh /path/to/boringssl}"
HEADER="$SRC/include/openssl/ssl.h"
IMPL="$SRC/ssl/ssl_versions.cc"
[ -f "$HEADER" ] && [ -f "$IMPL" ] || { echo "not a BoringSSL source tree: $SRC" >&2; exit 1; }

if ! grep -q "SSL_set_record_protocol_version" "$HEADER"; then
python3 - "$HEADER" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
anchor = "OPENSSL_EXPORT uint16_t SSL_get_min_proto_version(const SSL *ssl);\n"
if anchor not in s:
    sys.exit("patch_boringssl: anchor not found in ssl.h")
add = ("\n// netRector: sets the record layer protocol version of |ssl| (see TCPSocket::enableTLS bypassMode).\n"
       "OPENSSL_EXPORT int SSL_set_record_protocol_version(SSL *ssl, uint16_t version);\n"
       "#define TLS_CL_VERSION 0x0306\n")
open(p, "w").write(s.replace(anchor, anchor + add, 1))
PY
fi

if ! grep -q "^int SSL_set_record_protocol_version" "$IMPL"; then
if grep -q "FromOpaque(" "$IMPL"; then
# recent BoringSSL: SSL is an opaque struct, internals are reached through FromOpaque()
cat >> "$IMPL" <<'CC'

int SSL_set_record_protocol_version(SSL *ssl, uint16_t version) {
  auto *impl = FromOpaque(ssl);
  if (!impl->config) {
    return 0;
  }
  impl->s3->version = version;
  return 1;
}
CC
else
# older BoringSSL: struct ssl_st members are visible directly
cat >> "$IMPL" <<'CC'

int SSL_set_record_protocol_version(SSL *ssl, uint16_t version) {
  if (!ssl->config) {
    return 0;
  }
  ssl->s3->version = version;
  return 1;
}
CC
fi
fi

echo "BoringSSL patched: $SRC"
