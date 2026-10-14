#!/usr/bin/env bash
# Debug build with AddressSanitizer + UBSan (build-asan/netRector). For hunting memory errors.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BSSL="${BORINGSSL_DIR:-$ROOT/third_party/boringssl}"
OUT="$ROOT/build-asan"
mkdir -p "$OUT"
cd "$ROOT"
FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter"
gcc $FLAGS -c src/tlsf.c -o "$OUT/tlsf.o"
g++ -std=c++17 $FLAGS -Isrc -I"$BSSL/include" \
    src/clsBufferPool.cpp src/clsDNSLookup.cpp src/clsEpollReactor.cpp src/clsIntrusiveList.cpp src/clsListener.cpp src/clsUDPSocket.cpp \
    src/clsMultiplexedTunnel.cpp src/clsSendQueue.cpp src/clsServer.cpp src/clsSocketList.cpp \
    src/clsTCPSocket.cpp src/clsTLSContext.cpp src/clsTimer.cpp src/clsTimerManager.cpp main.cpp \
    "$OUT/tlsf.o" -L"$BSSL/lib" -lssl -lcrypto -lpthread -o "$OUT/netRector"
echo "built: $OUT/netRector"
