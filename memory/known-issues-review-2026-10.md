---
name: known-issues-review-2026-10
description: Status of the 2026-10-06 review findings after the fix pass; what is still open
metadata:
  type: project
---

The 2026-10-06 review (report/index.html, finding IDs A1..G12) was followed the same day by a fix pass. The report's "وضعیت پس از اصلاح" section has the per-finding status; the original sources are in backup/original-2026-10-06.tar.gz.

2026-10-22: an external review (ASD-STE100 report, 18 findings) was triaged; confirmed items fixed (shared read buffer corrupted by lingerDiscard from a nested close — real; sendv >64 iovec data loss; GC default; EMFILE busy loop; AddNewListener deadlock; send-queue hard cap; lazy SockInfo; EPOLLOUT kept armed; stream limit; DNS TTL + multi-IP; TLS worker live token; log handler; closeReason). Rejected by design: shared m_callbacksArg, non-graceful Server::stop, idle timeout 0. Details in report/index.html section "پاسخ به گزارش خارجی". The deep multiplexing stress suite is tests/muxstress (run_muxstress.sh asan) — run it after any change to TCPSocket/MultiplexedTunnel.

Still open after the fix pass:
- F1: SOCKS5 test programs have no authentication (bindIP now works; default listen is 127.0.0.1).
- DNS: IPv4 nameservers only, fixed source port, no negative cache, no TCP fallback for truncated answers.
- Not implemented (feature requests from the external review): AF_UNIX, bind to a source IP, TCP_USER_TIMEOUT/SO_LINGER setters, UDP send queue.
- Not tested: long soak runs. 10k keep-alive connections were benchmarked (bench/RESULTS.md).
- Possible further perf work: custom BIO so TLS reads use the shared reactor buffer (BoringSSL reads per record), MSG_ZEROCOPY for large sends.

**How to apply:** Before changing TCPSocket/MultiplexedTunnel, read the ownership and callback rules at the top of src/clsTCPSocket.h and src/clsMultiplexedTunnel.h, then rerun tests/run_tests.py (also with build-asan) — see [[build-environment]].

Related: [[architecture-map]], [[project-overview]].
