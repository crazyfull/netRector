#!/usr/bin/env bash
# Debug build with AddressSanitizer + UBSan (build-asan/netRector). For hunting memory errors.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-unused-parameter"
cmake -S "$ROOT" -B "$ROOT/build-asan" -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    ${BORINGSSL_DIR:+-DBORINGSSL_DIR="$BORINGSSL_DIR"}
cmake --build "$ROOT/build-asan" -j"$(nproc)"
echo "built: $ROOT/build-asan/netRector"
