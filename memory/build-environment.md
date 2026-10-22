---
name: build-environment
description: socketManager is Linux-only; build and test it inside the docker/Dockerfile image on the user's Mac
metadata:
  type: project
---

Linux-only (epoll, timerfd, eventfd, accept4). Needs BoringSSL (scripts/build_boringssl.sh -> third_party/boringssl, or BORINGSSL_DIR).

On the user's Mac (arm64): Docker Desktop must be started (`open -a Docker`). Image `socketmanager-dev` from docker/Dockerfile has qmake, g++ 13 and BoringSSL in /opt/boringssl.

    docker run --rm -v "$PWD":/work -w /work socketmanager-dev bash -c 'QT_SELECT=qt5 scripts/build.sh && scripts/build_asan.sh && python3 tests/run_tests.py --bin build-asan/epoll_new'

tests/run_tests.py: udp + topologies direct, tunnel, tls (async key), tls-ca, py, py-tls, and ext/ext-tls/extclient[-tls] with `--extserver tests/go_yamux/goyamux` (build it with tests/go_yamux/build.sh, needs the golang Docker image). yamux_server/test_selftest.py tests the Python server alone. Stress: `tests/muxstress/run_muxstress.sh asan [quick]` (needs tests/muxstress/build.sh and build/netRector; run containers with `-m 4g` — a run without a memory limit once killed the Docker VM). Benchmark: `bench/bench.sh 4 10` inside the container with `--ulimit nofile=1048576:1048576`; run it with nothing else using CPU (a concurrent build skewed nginx numbers once).

**Why:** nothing can be verified natively on macOS.
**How to apply:** never claim a change works without running the suite in the container; delete build/ and build-asan/ afterwards (they are root-owned build outputs).

Related: [[project-overview]].
