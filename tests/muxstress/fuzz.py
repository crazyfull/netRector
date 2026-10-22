#!/usr/bin/env python3
"""Yamux fuzzer for netRector.

server mode: connects to a yamux server (muxstress server / netRector server) and sends
malformed and hostile frames; after every case it checks that the server is still alive
by running a valid record echo on a fresh session. Nothing may crash.

client mode: acts as a fake yamux *server*; the netRector tunnel client
(`netRector client --server 127.0.0.1:9302 ...`) connects to it and receives garbage on
every reconnect. The client must drop the tunnel each time, keep running and reconnect;
run_muxstress.sh checks it is still alive afterwards and still works with a real server.

  python3 fuzz.py server --target 127.0.0.1:9300 [--rounds 200]
  python3 fuzz.py client --listen 127.0.0.1:9302 [--rounds 50]
"""
import argparse, os, random, socket, struct, sys, time, zlib

def hdr(t, f, sid, ln):
    return struct.pack("!BBHII", 0, t, f, sid, ln)

def record(seq, payload):
    return struct.pack("<III", len(payload), seq, zlib.crc32(payload) & 0xffffffff) + payload

def read_frames(s, timeout=1.0):
    s.settimeout(timeout)
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except socket.timeout:
        pass
    except OSError:
        pass
    return out

def alive_check(target):
    """valid session: SYN, one record, expect the same record echoed, FIN both ways."""
    s = socket.create_connection(target, timeout=5)
    s.sendall(hdr(1, 1, 1, 0))                      # SYN
    payload = os.urandom(300)
    rec = record(0, payload)
    s.sendall(hdr(0, 0, 1, len(rec)) + rec)
    s.sendall(hdr(1, 4, 1, 0))                      # FIN
    buf = b""
    deadline = time.time() + 5
    s.settimeout(1)
    while time.time() < deadline and rec not in buf:
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        except socket.timeout:
            continue
    s.close()
    return rec in buf

def rnd_frame(rng):
    t = rng.choice([0, 1, 2, 3, rng.randrange(256)])
    f = rng.choice([0, 1, 2, 4, 8, 3, 5, 12, 15, rng.randrange(65536)])
    sid = rng.choice([0, 1, 2, 3, 5, 0xFFFFFFFF, rng.randrange(1 << 32)])
    ln = rng.choice([0, 1, 12, 100, 65535, 256 * 1024, 256 * 1024 + 1, 0xFFFFFFFF, rng.randrange(1 << 20)])
    frame = hdr(t, f, sid, ln)
    if t == 0 and ln <= 300000:
        frame += os.urandom(min(ln, rng.choice([0, ln, ln // 2, 300000])))
    return frame

CASES = {
    "garbage": lambda rng: os.urandom(rng.randrange(1, 5000)),
    "bad_version": lambda rng: bytes([rng.randrange(1, 256)]) + os.urandom(11),
    "unknown_type": lambda rng: hdr(rng.randrange(4, 256), 0, 1, 0),
    "even_syn": lambda rng: hdr(1, 1, 2, 0),
    "syn_zero": lambda rng: hdr(1, 1, 0, 0),
    "dup_syn": lambda rng: hdr(1, 1, 1, 0) + hdr(1, 1, 1, 0),
    "data_zero_stream": lambda rng: hdr(0, 0, 0, 5) + b"hello",
    "window_exceed": lambda rng: hdr(1, 1, 1, 0) + hdr(0, 0, 1, 300000) + b"x" * 300000,
    "huge_length_truncated": lambda rng: hdr(1, 1, 1, 0) + hdr(0, 0, 1, 0xFFFFFFFF) + b"x" * 1000,
    "data_unknown_stream": lambda rng: hdr(0, 0, 77, 100) + b"y" * 100,
    "syn_flood": lambda rng: b"".join(hdr(1, 1, 2 * i + 1, 0) for i in range(5000)),
    "ping_storm": lambda rng: b"".join(hdr(2, 1, 0, i) for i in range(2000)),
    "window_update_overflow": lambda rng: hdr(1, 1, 1, 0) + b"".join(hdr(1, 0, 1, 0xFFFFFFFF) for _ in range(10)),
    "fin_rst_combo": lambda rng: hdr(1, 1, 1, 0) + hdr(1, 12, 1, 0) + hdr(0, 4, 1, 3) + b"abc",
    "data_after_fin": lambda rng: hdr(1, 1, 1, 0) + hdr(1, 4, 1, 0) + hdr(0, 0, 1, 10) + b"0123456789",
    "goaway_bad_code": lambda rng: hdr(3, 0, 0, 99),
    "goaway_normal_then_syn": lambda rng: hdr(3, 0, 0, 0) + hdr(1, 1, 1, 0),
    "half_header": lambda rng: hdr(1, 1, 1, 0)[:rng.randrange(1, 12)],
    "random_frames": lambda rng: b"".join(rnd_frame(rng) for _ in range(rng.randrange(1, 40))),
    "bad_record": lambda rng: hdr(1, 1, 1, 0) + hdr(0, 0, 1, 24) + struct.pack("<III", 0xFFFFFFF0, 0, 0) + b"z" * 12,
    "rst_then_data": lambda rng: hdr(1, 1, 1, 0) + hdr(1, 8, 1, 0) + hdr(0, 0, 1, 10) + b"0123456789",
}

def fuzz_server(args):
    rng = random.Random(args.seed)
    target = tuple(args.target.rsplit(":", 1)); target = (target[0], int(target[1]))
    fails = 0
    if not alive_check(target):
        print("FAIL: server not answering a valid session before fuzzing"); return 1
    names = list(CASES)
    for i in range(args.rounds):
        name = names[i % len(names)]
        payload = CASES[name](rng)
        try:
            s = socket.create_connection(target, timeout=5)
            # send in random chunks too
            pos = 0
            while pos < len(payload):
                n = rng.choice([len(payload) - pos, 1 + rng.randrange(64), 1 + rng.randrange(4096)])
                s.sendall(payload[pos:pos + n]); pos += n
            resp = read_frames(s, 0.3)
            s.close()
        except OSError as e:
            resp = b"<%s>" % type(e).__name__.encode()
        ok = alive_check(target)
        print("%-26s sent=%-7d resp=%-6d alive=%s" % (name, len(payload), len(resp), ok), flush=True)
        if not ok:
            fails += 1
            print("FAIL: server died / stopped answering after case %s" % name, flush=True)
            break
    print("fuzz server: %d rounds, %d failures" % (args.rounds, fails))
    return 1 if fails else 0

def fuzz_client(args):
    """fake yamux server: accept tunnel connections and send hostile frames."""
    rng = random.Random(args.seed)
    lh, lp = args.listen.rsplit(":", 1)
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((lh, int(lp))); ls.listen(64); ls.settimeout(args.timeout)
    names = list(CASES)
    served = 0
    print("fuzz client: fake server on %s, waiting for %d tunnels" % (args.listen, args.rounds), flush=True)
    while served < args.rounds:
        try:
            c, _ = ls.accept()
        except socket.timeout:
            print("fuzz client: no more connections (client gave up or died?) after %d" % served); break
        name = names[served % len(names)]
        try:
            # read whatever the client sends first (SYNs), then answer with hostile bytes
            c.settimeout(0.3)
            try: c.recv(65536)
            except OSError: pass
            c.sendall(CASES[name](rng))
            c.settimeout(0.5)
            try:
                while c.recv(65536): pass
            except OSError: pass
            c.close()
        except OSError:
            pass
        served += 1
        print("%-26s served tunnel #%d" % (name, served), flush=True)
    return 0

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["server", "client"])
    ap.add_argument("--target", default="127.0.0.1:9300")
    ap.add_argument("--listen", default="127.0.0.1:9302")
    ap.add_argument("--rounds", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=20)
    a = ap.parse_args()
    sys.exit(fuzz_server(a) if a.mode == "server" else fuzz_client(a))
