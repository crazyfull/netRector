#!/usr/bin/env bash
# Build BoringSSL (static libssl.a / libcrypto.a) into libs/boringssl.
# The BoringSSL sources are patched with scripts/patch_boringssl.sh before the build.
# Needs: git, cmake (>= 3.22), ninja, a C++17 compiler.
#
#   scripts/build_boringssl.sh            # -> libs/boringssl/{include,lib}
#   scripts/build_boringssl.sh /opt/bssl  # custom prefix (then: cmake -DBORINGSSL_DIR=/opt/bssl)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${1:-$ROOT/libs/boringssl}"
SRC="$(mktemp -d)"
trap 'rm -rf "$SRC"' EXIT

git clone --depth 1 https://github.com/google/boringssl.git "$SRC/boringssl"
# netRector patch (SSL_set_record_protocol_version / TLS_CL_VERSION): must be applied before building
"$ROOT/scripts/patch_boringssl.sh" "$SRC/boringssl"
cmake -GNinja -S "$SRC/boringssl" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release
ninja -C "$SRC/build" crypto ssl

mkdir -p "$PREFIX/lib"
cp "$(find "$SRC/build" -name libssl.a | head -1)" "$PREFIX/lib/"
cp "$(find "$SRC/build" -name libcrypto.a | head -1)" "$PREFIX/lib/"
rm -rf "$PREFIX/include"
cp -r "$SRC/boringssl/include" "$PREFIX/include"

echo "BoringSSL installed in $PREFIX"
