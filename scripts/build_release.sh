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
        set -e
        rm -rf /tmp/b
        cmake -S /work -B /tmp/b -DCMAKE_BUILD_TYPE=Release -DNETRECTOR_STATIC=ON >/dev/null
        cmake --build /tmp/b -j\$(nproc)
        strip /tmp/b/netRector && mkdir -p /work/build/$out && cp /tmp/b/netRector /work/build/$out/netRector"
    echo "built: build/$out/netRector"
done
