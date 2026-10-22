#!/usr/bin/env bash
# Build static release binaries for Linux x86-64 and arm64 with Docker (needs internet
# the first time to pull ubuntu:24.04 and clone BoringSSL). Output: build/linux-<arch>/netRector
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
for arch in "${@:-amd64 arm64}"; do
    out="linux-x86_64"; [ "$arch" = "arm64" ] && out="linux-arm64"
    docker build --platform "linux/$arch" -t "netrector-dev-$arch" docker/
    docker run --rm --platform "linux/$arch" -v "$ROOT":/work -w /work "netrector-dev-$arch" bash -c "
        export QT_SELECT=qt5
        rm -rf /tmp/b && mkdir /tmp/b && cd /tmp/b
        qmake /work/netRector.pro CONFIG+=release >/dev/null && make -j\$(nproc) 2>&1 | grep -E ' error ' || true
        strip netRector && mkdir -p /work/build/$out && cp netRector /work/build/$out/netRector"
    echo "built: build/$out/netRector"
done
