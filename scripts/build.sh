#!/usr/bin/env bash
# Build the library and the example app with CMake: build/libnetRector.a (in build/lib) and build/netRector.
# Run on Linux (or inside docker/Dockerfile). BoringSSL: libs/boringssl, or BORINGSSL_DIR=/path.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release ${BORINGSSL_DIR:+-DBORINGSSL_DIR="$BORINGSSL_DIR"}
cmake --build "$ROOT/build" -j"$(nproc)"
echo "built: $ROOT/build/netRector"
