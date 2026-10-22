#!/usr/bin/env bash
# Build the test binary (build/netRector) with qmake. Run on Linux (or inside docker/Dockerfile).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build"
cd "$ROOT/build"
qmake "$ROOT/netRector.pro" CONFIG+=release
make -j"$(nproc)"
echo "built: $ROOT/build/netRector"
