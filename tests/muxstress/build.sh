#!/usr/bin/env bash
# Build muxstress (and the ASan variant) against ../../src. Run on Linux (or inside the dev container).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."
BSSL="${BORINGSSL_DIR:-$ROOT/third_party/boringssl}"
SRC="$ROOT/src/clsBufferPool.cpp $ROOT/src/clsDNSLookup.cpp $ROOT/src/clsEpollReactor.cpp $ROOT/src/clsIntrusiveList.cpp $ROOT/src/clsListener.cpp $ROOT/src/clsLog.cpp $ROOT/src/clsUDPSocket.cpp $ROOT/src/clsMultiplexedTunnel.cpp $ROOT/src/clsSendQueue.cpp $ROOT/src/clsServer.cpp $ROOT/src/clsSocketList.cpp $ROOT/src/clsTCPSocket.cpp $ROOT/src/clsTLSContext.cpp $ROOT/src/clsTimer.cpp $ROOT/src/clsTimerManager.cpp"
gcc -O2 -c "$ROOT/src/tlsf.c" -o /tmp/tlsf_ms.o
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -I"$ROOT/src" -I"$BSSL/include" $SRC "$HERE/muxstress.cpp" /tmp/tlsf_ms.o -L"$BSSL/lib" -lssl -lcrypto -lpthread -o "$HERE/muxstress"
if [ "${ASAN:-1}" = "1" ]; then
  gcc -g -O1 -fsanitize=address,undefined -c "$ROOT/src/tlsf.c" -o /tmp/tlsf_ms_asan.o
  g++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wno-unused-parameter -I"$ROOT/src" -I"$BSSL/include" $SRC "$HERE/muxstress.cpp" /tmp/tlsf_ms_asan.o -L"$BSSL/lib" -lssl -lcrypto -lpthread -o "$HERE/muxstress-asan"
fi
echo "built: $HERE/muxstress $([ "${ASAN:-1}" = "1" ] && echo "+ muxstress-asan")"
