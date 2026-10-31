#!/usr/bin/env bash
# Build BoringSSL (static libssl.a / libcrypto.a) into libs/boringssl.
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
cmake -GNinja -S "$SRC/boringssl" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release
ninja -C "$SRC/build" crypto ssl

mkdir -p "$PREFIX/lib"
cp "$(find "$SRC/build" -name libssl.a | head -1)" "$PREFIX/lib/"
cp "$(find "$SRC/build" -name libcrypto.a | head -1)" "$PREFIX/lib/"
rm -rf "$PREFIX/include"
cp -r "$SRC/boringssl/include" "$PREFIX/include"

echo "BoringSSL installed in $PREFIX"
