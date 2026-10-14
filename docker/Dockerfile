# Build/test environment for netRector (Linux only: epoll, timerfd, eventfd).
#
#   docker build -t netrector-dev docker/
#   docker run --rm -it -v "$PWD":/work -w /work netrector-dev bash
#   # inside:  qmake netRector.pro -o build/Makefile && make -C build -j"$(nproc)"
#
# BoringSSL is built once into /opt/boringssl (headers + static libs).
FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        qt5-qmake qtchooser \
        python3 curl openssl iproute2 procps file \
        nginx wrk \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/google/boringssl.git /tmp/boringssl \
    && cmake -GNinja -S /tmp/boringssl -B /tmp/boringssl/build -DCMAKE_BUILD_TYPE=Release \
    && ninja -C /tmp/boringssl/build crypto ssl \
    && mkdir -p /opt/boringssl/lib \
    && cp "$(find /tmp/boringssl/build -name libssl.a | head -1)" /opt/boringssl/lib/ \
    && cp "$(find /tmp/boringssl/build -name libcrypto.a | head -1)" /opt/boringssl/lib/ \
    && cp -r /tmp/boringssl/include /opt/boringssl/include \
    && rm -rf /tmp/boringssl

ENV BORINGSSL_DIR=/opt/boringssl
WORKDIR /work
