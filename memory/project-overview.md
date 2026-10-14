---
name: project-overview
description: What netRector (formerly socketManager) is, its goal as a general high-performance network library, and the test program modes
metadata:
  type: project
---

The project is called **netRector** (renamed from socketManager on 2026-10-13; qmake project `netRector.pro`, target `netRector`; the folder is still `socketManager`). It is a C++17 epoll-based *general-purpose* networking library — the user's stated goal is nginx-class performance for many concurrent connections, to reuse in other network projects; tunneling/proxying is only one use case, so do not optimise for the proxy alone.

The root test program `main.cpp` has modes `client` / `server` (SOCKS5-over-Yamux tunnel), `socks5` (direct proxy), `http` (benchmark server: static reply via sendNoCopy or a file via sendFile, optional TLS, `--tls-threads`), `udp` (UDP echo). The user allows root-level test files to be rewritten freely; library code in src/ should keep its structure and only get fixes/perf work. `yamux_server/` is a separate Python Yamux test server that must stay protocol-compatible with the C++ client.

The project is not a git repository (as of 2026-10-06).

Related: [[architecture-map]], [[build-environment]], [[known-issues-review-2026-10]].
