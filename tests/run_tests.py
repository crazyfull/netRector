#!/usr/bin/env python3
"""Integration tests for the netRector test binary (Linux only).

Starts local destination servers, launches the proxy processes in several
topologies and drives them with a small blocking SOCKS5 client:

  direct   : netRector socks5
  tunnel   : netRector client  <-yamux/TCP->  netRector server
  tls      : netRector client  <-yamux/TLS->  netRector server   (--insecure)
  tls-ca   : same, client verifies the certificate (--ca, --sni localhost)
  py       : netRector client  <-yamux/TCP->  yamux_server/server.py
  py-tls   : netRector client  <-yamux/TLS->  yamux_server/server.py
  ext[-tls]: netRector client  <-yamux->      any other server given with --extserver

usage:  python3 tests/run_tests.py [--bin build/netRector] [--only tunnel,tls] [--threads N] [-v]
"""
import argparse
import hashlib
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"


# --------------------------------------------------------------------------- helpers
def free_port():
    s = socket.socket()
    s.bind((HOST, 0))
    p = s.getsockname()[1]
    s.close()
    return p


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed after %d of %d bytes" % (len(buf), n))
        buf += chunk
    return bytes(buf)


def recv_all(sock):
    h = hashlib.sha256()
    total = 0
    while True:
        chunk = sock.recv(256 * 1024)
        if not chunk:
            return h.hexdigest(), total
        h.update(chunk)
        total += len(chunk)


def payload(seed, size):
    return random.Random(seed).randbytes(size)


def rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def fd_count(pid):
    try:
        return len(os.listdir("/proc/%d/fd" % pid))
    except OSError:
        return -1


def wait_port(port, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection((HOST, port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


# --------------------------------------------------------------------------- destination servers
class ThreadedServer:
    def __init__(self, handler, family=socket.AF_INET):
        self.sock = socket.socket(family)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("::1" if family == socket.AF_INET6 else HOST, 0))
        self.sock.listen(1024)
        self.port = self.sock.getsockname()[1]
        self.handler = handler
        self.active = 0
        self.total = 0
        self.lock = threading.Lock()
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._run, args=(c,), daemon=True).start()

    def _run(self, c):
        with self.lock:
            self.active += 1
            self.total += 1
        try:
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.handler(c)
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass
            with self.lock:
                self.active -= 1


def echo_handler(c):
    while True:
        d = c.recv(65536)
        if not d:
            return
        c.sendall(d)


def source_handler(c):
    """client sends seed(8) + size(8); server sends that many pseudo random bytes and closes."""
    seed, size = struct.unpack("!QQ", recv_exact(c, 16))
    data = payload(seed, size)
    c.sendall(data)


def sink_handler(c):
    """reads until EOF (needs half-close through the proxy), answers 'sha256 length'."""
    digest, total = recv_all(c)
    c.sendall(("%s %d" % (digest, total)).encode())


# --------------------------------------------------------------------------- socks5 client
class SocksError(Exception):
    def __init__(self, rep):
        super().__init__("socks reply 0x%02x" % rep)
        self.rep = rep


def socks_connect(proxy_port, host, port, use_domain=False, timeout=30.0):
    s = socket.create_connection((HOST, proxy_port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(b"\x05\x01\x00")
    if recv_exact(s, 2) != b"\x05\x00":
        s.close()
        raise RuntimeError("bad greeting reply")
    if ":" in host:
        req = b"\x05\x01\x00\x04" + socket.inet_pton(socket.AF_INET6, host) + struct.pack("!H", port)
    elif use_domain:
        hb = host.encode()
        req = b"\x05\x01\x00\x03" + bytes([len(hb)]) + hb + struct.pack("!H", port)
    else:
        req = b"\x05\x01\x00\x01" + socket.inet_aton(host) + struct.pack("!H", port)
    s.sendall(req)
    rep = recv_exact(s, 10)
    if rep[0] != 5 or rep[1] != 0:
        s.close()
        raise SocksError(rep[1])
    return s


# --------------------------------------------------------------------------- topology
class Topology:
    def __init__(self, name, args, servers):
        self.name = name
        self.args = args
        self.servers = servers
        self.procs = {}
        self.proxy_port = free_port()
        self.tunnel_port = free_port()
        self.logs = {}

    def _spawn(self, key, cmd):
        log = open(os.path.join(self.args.logdir, "%s-%s.log" % (self.name, key)), "wb")
        self.logs[key] = log.name
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        self.procs[key] = p
        return p

    def server_cmd(self):
        b = self.args.bin
        th = ["--threads", str(self.args.threads)]
        extra = ["-v"] if self.args.verbose else []
        cert, key = self.args.cert, self.args.key
        n = self.name
        if n in ("tunnel", "extclient"):
            return [b, "server", "--listen", "%s:%d" % (HOST, self.tunnel_port)] + th + extra
        if n in ("tls", "tls-ca", "extclient-tls"):
            # tls: private key operations on worker threads; tls-ca: synchronous
            async_key = ["--tls-threads", "2"] if n == "tls" else []
            return [b, "server", "--listen", "%s:%d" % (HOST, self.tunnel_port), "--cert", cert, "--key", key] + async_key + th + extra
        if n == "py":
            return [sys.executable, self.args.pyserver, "--listen", "%s:%d" % (HOST, self.tunnel_port)] + extra
        if n == "py-tls":
            return [sys.executable, self.args.pyserver, "--listen", "%s:%d" % (HOST, self.tunnel_port),
                    "--tls-cert", cert, "--tls-key", key] + extra
        # optional: any other yamux server binary with the same command line (e.g. one built on hashicorp/yamux)
        if n == "ext":
            return [self.args.extserver, "--listen", "%s:%d" % (HOST, self.tunnel_port)]
        if n == "ext-tls":
            return [self.args.extserver, "--listen", "%s:%d" % (HOST, self.tunnel_port),
                    "--tls-cert", cert, "--tls-key", key]
        return None

    def client_cmd(self):
        b = self.args.bin
        th = ["--threads", str(self.args.threads)]
        extra = ["-v"] if self.args.verbose else []
        n = self.name
        listen = ["--listen", "%s:%d" % (HOST, self.proxy_port)]
        if n == "direct":
            return [b, "socks5"] + listen + th + extra
        # optional: another yamux client (e.g. hashicorp/yamux) against the C++ server
        if n in ("extclient", "extclient-tls"):
            cmd = [self.args.extserver, "--client", "--server", "%s:%d" % (HOST, self.tunnel_port)] + listen
            return cmd + (["--insecure"] if n.endswith("tls") else [])
        base = [b, "client", "--server", "%s:%d" % (HOST, self.tunnel_port), "--keepalive", "5"] + listen + th + extra
        if n in ("tls", "py-tls", "ext-tls"):
            return base + ["--insecure"]
        if n == "tls-ca":
            return base + ["--tls", "--ca", self.args.cert, "--sni", "localhost"]
        return base

    def start_server(self):
        cmd = self.server_cmd()
        if cmd:
            self._spawn("server", cmd)
            if not wait_port(self.tunnel_port):
                raise RuntimeError("tunnel server did not start (see %s)" % self.logs["server"])

    def start(self):
        self.start_server()
        self._spawn("client", self.client_cmd())
        if not wait_port(self.proxy_port):
            raise RuntimeError("proxy did not start (see %s)" % self.logs["client"])
        self.wait_tunnel()

    def wait_tunnel(self, timeout=15.0):
        """wait until a request through the proxy succeeds (tunnel established on every shard)."""
        end = time.time() + timeout
        ok_needed = max(8, self.args.threads * 8)
        ok = 0
        while time.time() < end:
            try:
                s = socks_connect(self.proxy_port, HOST, self.servers["echo"].port, timeout=3)
                s.close()
                ok += 1
                if ok >= ok_needed:
                    return
            except (SocksError, OSError, EOFError, RuntimeError):
                ok = 0
                time.sleep(0.1)
        raise RuntimeError("tunnel not usable after %.0fs" % timeout)

    def alive(self):
        return all(p.poll() is None for p in self.procs.values())

    def stop(self):
        codes = {}
        for key, p in self.procs.items():
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for key, p in self.procs.items():
            try:
                codes[key] = p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                codes[key] = "killed (did not exit on SIGTERM)"
        return codes


# --------------------------------------------------------------------------- tests
def t_echo(top):
    for use_domain in (False, True):
        s = socks_connect(top.proxy_port, "localhost" if use_domain else HOST, top.servers["echo"].port, use_domain)
        msg = os.urandom(3000)
        s.sendall(msg)
        assert recv_exact(s, len(msg)) == msg, "echo mismatch"
        s.close()


def t_ipv6(top):
    srv = top.servers.get("echo6")
    if not srv:
        return "skipped (no IPv6 loopback)"
    s = socks_connect(top.proxy_port, "::1", srv.port)
    msg = os.urandom(5000)
    s.sendall(msg)
    assert recv_exact(s, len(msg)) == msg, "echo mismatch"
    s.close()


def t_download(top, size=64 * 1024 * 1024):
    seed = random.getrandbits(32)
    s = socks_connect(top.proxy_port, HOST, top.servers["source"].port)
    s.sendall(struct.pack("!QQ", seed, size))
    digest, total = recv_all(s)
    s.close()
    assert total == size, "download size %d != %d" % (total, size)
    assert digest == hashlib.sha256(payload(seed, size)).hexdigest(), "download hash mismatch"


def t_upload_halfclose(top, size=64 * 1024 * 1024):
    data = payload(random.getrandbits(32), size)
    s = socks_connect(top.proxy_port, HOST, top.servers["sink"].port)
    s.sendall(data)
    s.shutdown(socket.SHUT_WR)          # half-close: the answer must still arrive
    reply = b""
    while True:
        d = s.recv(4096)
        if not d:
            break
        reply += d
    s.close()
    assert reply, "no reply after half-close"
    digest, total = reply.decode().split()
    assert int(total) == size, "upload size %s != %d" % (total, size)
    assert digest == hashlib.sha256(data).hexdigest(), "upload hash mismatch"


def t_concurrent(top, streams=100, size=512 * 1024):
    errors = []

    def worker(i):
        try:
            s = socks_connect(top.proxy_port, HOST, top.servers["echo"].port, timeout=60)
            data = payload(i, size)
            got = bytearray()

            def reader():
                try:
                    while len(got) < size:
                        d = s.recv(65536)
                        if not d:
                            break
                        got.extend(d)
                except OSError:
                    pass

            rt = threading.Thread(target=reader)
            rt.start()
            s.sendall(data)
            rt.join(60)
            s.close()
            if bytes(got) != data:
                errors.append("stream %d: got %d of %d bytes or mismatch" % (i, len(got), size))
        except Exception as e:  # noqa: BLE001
            errors.append("stream %d: %r" % (i, e))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(streams)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(120)
    assert not errors, "; ".join(errors[:5])


def t_refused(top):
    port = free_port()  # nothing listens here
    try:
        s = socks_connect(top.proxy_port, HOST, port)
        s.close()
    except SocksError as e:
        assert e.rep == 0x05, "expected REP 05 (refused), got %02x" % e.rep
        return
    raise AssertionError("connect to closed port succeeded")


def t_dns_failure(top):
    try:
        s = socks_connect(top.proxy_port, "no-such-host.invalid", 80, use_domain=True)
        s.close()
    except SocksError as e:
        assert e.rep in (0x01, 0x04), "unexpected REP %02x" % e.rep
        return
    raise AssertionError("connect to invalid host succeeded")


def t_slow_reader(top, size=48 * 1024 * 1024):
    """the proxy must not buffer the whole download when the client reads slowly."""
    pids = [p.pid for p in top.procs.values()]
    before = sum(rss_kb(p) for p in pids)
    seed = random.getrandbits(32)
    s = socks_connect(top.proxy_port, HOST, top.servers["source"].port)
    s.sendall(struct.pack("!QQ", seed, size))
    time.sleep(3.0)                     # do not read: everything upstream has to stall
    peak = sum(rss_kb(p) for p in pids)
    digest, total = recv_all(s)
    s.close()
    assert total == size and digest == hashlib.sha256(payload(seed, size)).hexdigest(), "slow reader data mismatch"
    grown_mb = (peak - before) / 1024.0
    assert grown_mb < 16, "proxy buffered %.1f MB while the client was not reading (no backpressure)" % grown_mb
    return "rss growth %.1f MB" % grown_mb


def t_abort(top):
    """client drops mid-download; the destination connection must be closed and others keep working."""
    if top.name.startswith("extclient"):
        return "skipped (external client cannot reset a single stream)"
    src = top.servers["source"]
    s = socks_connect(top.proxy_port, HOST, src.port)
    s.sendall(struct.pack("!QQ", 1, 200 * 1024 * 1024))
    recv_exact(s, 1024 * 1024)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()                           # RST
    t_echo(top)
    end = time.time() + 10
    while time.time() < end and src.active > 0:
        time.sleep(0.1)
    assert src.active == 0, "destination connection still open after client abort (active=%d)" % src.active


def t_many_short(top, count=2000):
    pids = {k: p.pid for k, p in top.procs.items()}
    before = {k: fd_count(pid) for k, pid in pids.items()}
    for i in range(count):
        s = socks_connect(top.proxy_port, HOST, top.servers["echo"].port)
        s.sendall(b"x" * 100)
        recv_exact(s, 100)
        s.close()
    time.sleep(1.5)
    after = {k: fd_count(pid) for k, pid in pids.items()}
    for k in pids:
        assert after[k] - before[k] <= 4, "%s fd leak: %d -> %d" % (k, before[k], after[k])
    return "fds %s" % after


def t_reconnect(top):
    """kill the tunnel server: requests fail cleanly; after restart the client reconnects by itself."""
    if "server" not in top.procs:
        return "skipped (no tunnel)"
    if top.name.startswith("extclient"):
        return "skipped (external client has no reconnect)"
    # an open stream must be closed when the tunnel dies
    s = socks_connect(top.proxy_port, HOST, top.servers["echo"].port)
    top.procs["server"].kill()
    top.procs["server"].wait()
    s.settimeout(10)
    try:
        d = s.recv(10)
        assert d == b"", "expected close after tunnel loss"
    except ConnectionError:
        pass
    s.close()
    try:
        socks_connect(top.proxy_port, HOST, top.servers["echo"].port, timeout=5).close()
        raise AssertionError("request succeeded without a tunnel")
    except (SocksError, OSError, EOFError):
        pass
    top.start_server()
    top.wait_tunnel(20)
    t_echo(top)


TESTS = [
    ("echo (IPv4 + domain)", t_echo),
    ("IPv6 destination (ATYP 04)", t_ipv6),
    ("download 64MB + hash", t_download),
    ("upload 64MB + half-close", t_upload_halfclose),
    ("100 concurrent streams x 512KB", t_concurrent),
    ("connection refused -> REP 05", t_refused),
    ("DNS failure -> error reply", t_dns_failure),
    ("slow reader / backpressure", t_slow_reader),
    ("client abort mid-download", t_abort),
    ("2000 short connections / fd leak", t_many_short),
    ("tunnel loss + reconnect", t_reconnect),
]


def test_udp(args):
    """UDP echo through UDPSocket: single datagrams, a burst of 500 (recvmmsg batching), 60KB datagram, IPv6."""
    port = free_port()
    log = open(os.path.join(args.logdir, "udp.log"), "wb")
    p = subprocess.Popen([args.bin, "udp", "--listen", "127.0.0.1:%d" % port, "--threads", str(args.threads)],
                         stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    time.sleep(0.5)
    ok = True
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(5)
        s.sendto(b"hello", ("127.0.0.1", port))
        assert s.recvfrom(65535)[0] == b"hello", "single echo"
        big = os.urandom(60000)
        s.sendto(big, ("127.0.0.1", port))
        assert s.recvfrom(65535)[0] == big, "60KB datagram"
        # burst of 2000: a reader thread drains the client socket while sending, because the
        # default 212KB client receive buffer only holds ~240 small datagrams (skb overhead)
        sent = [("%05d" % i).encode() * 20 for i in range(2000)]
        got = set()

        def reader():
            while len(got) < len(sent):
                try:
                    got.add(s.recvfrom(65535)[0])
                except socket.timeout:
                    return

        rt = threading.Thread(target=reader)
        rt.start()
        for i, d in enumerate(sent):
            s.sendto(d, ("127.0.0.1", port))
            if i % 200 == 199:
                time.sleep(0.005)   # let the reader keep up with the 212KB client buffer
        rt.join(10)
        assert len(got) >= len(sent) * 0.98, "burst: %d of %d echoed" % (len(got), len(sent))
        s.close()
        print("  PASS  UDP echo (single, 60KB, burst %d/%d)" % (len(got), len(sent)), flush=True)
    except Exception as e:  # noqa: BLE001
        ok = False
        print("  FAIL  UDP echo: %r" % e, flush=True)
    p.send_signal(signal.SIGTERM)
    try:
        code = p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill(); code = "killed"
    if code != 0:
        ok = False
        print("  FAIL  UDP clean shutdown: %s" % code, flush=True)
    else:
        print("  PASS  UDP clean shutdown", flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "netRector"))
    ap.add_argument("--pyserver", default=os.path.join(ROOT, "yamux_server", "server.py"))
    ap.add_argument("--extserver", default="", help="external yamux server binary (topologies ext, ext-tls)")
    ap.add_argument("--only", default="")
    ap.add_argument("--tests", default="", help="substring filter for test names")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--logdir", default="/tmp/netrector-tests")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.logdir, exist_ok=True)
    args.cert = os.path.join(args.logdir, "cert.pem")
    args.key = os.path.join(args.logdir, "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                    "-keyout", args.key, "-out", args.cert], check=True, capture_output=True)

    servers = {
        "echo": ThreadedServer(echo_handler),
        "source": ThreadedServer(source_handler),
        "sink": ThreadedServer(sink_handler),
    }
    try:
        servers["echo6"] = ThreadedServer(echo_handler, socket.AF_INET6)
    except OSError:
        pass

    names = ["direct", "tunnel", "tls", "tls-ca"]
    if os.path.exists(args.pyserver):
        names += ["py", "py-tls"]
    if args.extserver:
        names += ["ext", "ext-tls", "extclient", "extclient-tls"]
    if args.only:
        names = [n for n in args.only.split(",") if n]

    failed = 0
    if not args.only or "udp" in args.only:
        print("=== udp (threads=%d) ===" % args.threads, flush=True)
        if not test_udp(args):
            failed += 1
    names = [n for n in names if n != "udp"]
    for name in names:
        print("=== %s (threads=%d) ===" % (name, args.threads), flush=True)
        top = Topology(name, args, servers)
        try:
            top.start()
        except Exception as e:  # noqa: BLE001
            print("  FAIL  start: %s" % e, flush=True)
            failed += 1
            top.stop()
            continue

        for title, fn in TESTS:
            if args.tests and args.tests not in title:
                continue
            t0 = time.time()
            try:
                note = fn(top)
                if not top.alive():
                    raise AssertionError("a proxy process died (see %s)" % ", ".join(top.logs.values()))
                print("  PASS  %-36s %5.1fs %s" % (title, time.time() - t0, note or ""), flush=True)
            except Exception as e:  # noqa: BLE001
                failed += 1
                print("  FAIL  %-36s %5.1fs %r" % (title, time.time() - t0, e), flush=True)
                if not top.alive():
                    break

        codes = top.stop()
        # external helper binaries have no SIGTERM handler; only our own processes must exit with 0
        external = {"ext": ["server"], "ext-tls": ["server"], "extclient": ["client"], "extclient-tls": ["client"]}.get(name, [])
        bad = {k: c for k, c in codes.items() if c != 0 and not (k in external and c == -signal.SIGTERM)}
        if bad:
            failed += 1
            print("  FAIL  clean shutdown: exit codes %s (logs: %s)" % (bad, ", ".join(top.logs.values())), flush=True)
        else:
            print("  PASS  clean shutdown", flush=True)

    print("\n%s" % ("ALL PASSED" if failed == 0 else "%d FAILED" % failed), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
