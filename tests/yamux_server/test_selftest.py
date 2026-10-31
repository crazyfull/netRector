#!/usr/bin/env python3
"""Self-test for server.py.

Starts the server in-process on a random port together with a few local
destination servers, and drives it with a minimal asyncio yamux client that
implements the same wire protocol. The client also watches the server's
behaviour on every frame (window never exceeded, Data frames <= 32 KiB, no more
window returned than was sent, ...) and reports violations.

Usage: python3 test_selftest.py        (exit code 0 only if every check passes)
"""
from __future__ import annotations

import asyncio
import contextlib
import hashlib
import os
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import server as srv  # noqa: E402

HDR = struct.Struct(">BBHII")
DATA, WINUP, PING, GOAWAY = 0, 1, 2, 3
SYN, ACK, FIN, RST = 1, 2, 4, 8
WINDOW = 256 * 1024
MAX_FRAME = 32 * 1024
KB, MB = 1000, 1024 * 1024
CHECK_TIMEOUT = 120.0


class Skip(Exception):
    pass


def expect(cond, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


async def within(awaitable, seconds: float = 10.0, what: str = "operation"):
    try:
        return await asyncio.wait_for(awaitable, seconds)
    except (asyncio.TimeoutError, TimeoutError):
        raise AssertionError(f"timeout after {seconds}s waiting for {what}") from None


async def until(pred, seconds: float = 5.0, what: str = "condition") -> None:
    deadline = time.monotonic() + seconds
    while not pred():
        if time.monotonic() > deadline:
            raise AssertionError(f"timeout after {seconds}s waiting for {what}")
        await asyncio.sleep(0.01)


def socks_request(host: str, port: int, atyp: int = 1, cmd: int = 1, ver: int = 5) -> bytes:
    if atyp == 1:
        addr = socket.inet_aton(host)
    elif atyp == 4:
        addr = socket.inet_pton(socket.AF_INET6, host)
    elif atyp == 3:
        addr = bytes([len(host)]) + host.encode()
    else:
        addr = b"\x00\x00\x00\x00"
    return bytes([ver, cmd, 0, atyp]) + addr + struct.pack(">H", port)


def socks_reply(rep: int) -> bytes:
    return bytes([5, rep, 0, 1, 0, 0, 0, 0, 0, 0])


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# ---------------------------------------------------------------------------------
# Minimal yamux client
# ---------------------------------------------------------------------------------

class CStream:
    def __init__(self, client: "YClient", sid: int, auto_window: bool, syn_on_data: bool):
        self.c, self.id = client, sid
        self.auto_window = auto_window    # return window as soon as data arrives
        self.pending_syn = syn_on_data    # first Data frame carries SYN
        self.send_window = WINDOW
        self.recv_window = WINDOW         # what the server may still send us
        self.unacked = 0
        self.sent = 0                     # payload bytes sent to the server
        self.returned = 0                 # window the server gave back
        self.received = 0                 # payload bytes received
        self.buf = bytearray()
        self.acked = self.fin = self.rst = False
        self.rx, self.tx = asyncio.Event(), asyncio.Event()

    def _dead(self) -> bool:
        return self.rst or self.c.closed.is_set()

    async def write(self, data: bytes, frame: int = MAX_FRAME) -> None:
        view = memoryview(data)
        while view:
            while self.send_window <= 0:
                expect(not self._dead(), f"stream {self.id}: reset/closed while writing")
                self.tx.clear()
                await self.tx.wait()
            n = min(len(view), self.send_window, frame)
            flags, self.pending_syn = (SYN if self.pending_syn else 0), False
            self.send_window -= n
            self.sent += n
            await self.c.send(DATA, flags, self.id, n, view[:n])
            view = view[n:]

    async def _wait(self, pred) -> None:
        while not pred() and not self._dead():
            self.rx.clear()
            await self.rx.wait()

    async def read(self, n: int) -> bytes:
        """Exactly n bytes; fails if the stream ends first."""
        await self._wait(lambda: len(self.buf) >= n or self.fin)
        expect(len(self.buf) >= n, f"stream {self.id}: wanted {n} bytes, got {len(self.buf)} "
                                   f"(fin={self.fin} rst={self.rst} closed={self.c.closed.is_set()})")
        data = bytes(self.buf[:n])
        del self.buf[:n]
        return data

    async def read_some(self) -> bytes:
        """Whatever is buffered; b"" at FIN."""
        await self._wait(lambda: self.buf or self.fin)
        expect(self.buf or self.fin, f"stream {self.id}: reset/closed while reading")
        data, self.buf = bytes(self.buf), bytearray()
        return data

    async def read_to_fin(self) -> bytes:
        out = bytearray()
        while True:
            chunk = await self.read_some()
            if not chunk:
                return bytes(out)
            out += chunk

    async def wait_fin(self) -> None:
        await self._wait(lambda: self.fin)
        expect(self.fin and not self.rst, f"stream {self.id}: expected FIN (rst={self.rst})")

    async def wait_rst(self) -> None:
        await self._wait(lambda: False)
        expect(self.rst, f"stream {self.id}: expected RST, tunnel closed instead")

    async def send_fin(self) -> None:
        await self.c.send(WINUP, FIN, self.id, 0)

    async def send_rst(self) -> None:
        await self.c.send(WINUP, RST, self.id, 0)

    def grant(self, n: int) -> None:
        self.recv_window += n
        self.c.send_nowait(WINUP, 0, self.id, n)


class YClient:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.r, self.w = reader, writer
        self.streams: dict[int, CStream] = {}
        self.next_id = 1
        self.lock = asyncio.Lock()
        self.violations: list[str] = []
        self.pongs: asyncio.Queue = asyncio.Queue()
        self.server_pings: list[int] = []
        self.answer_pings = True
        self.goaway = None
        self.closed = asyncio.Event()     # tunnel EOF / error
        self.task = asyncio.create_task(self._loop())

    @classmethod
    async def connect(cls, port: int, ssl_ctx=None) -> "YClient":
        r, w = await asyncio.open_connection(
            "127.0.0.1", port, ssl=ssl_ctx, limit=WINDOW,
            server_hostname="localhost" if ssl_ctx else None)
        return cls(r, w)

    def send_nowait(self, typ, flags, sid, length, payload=b"") -> None:
        if not self.w.transport.is_closing():
            self.w.write(HDR.pack(0, typ, flags, sid, length) + bytes(payload))

    async def send(self, typ, flags, sid, length, payload=b"", ver=0) -> None:
        async with self.lock:
            self.w.write(HDR.pack(ver, typ, flags, sid, length) + bytes(payload))
            await self.w.drain()

    async def open(self, sid=None, auto_window=True, syn_on_data=False) -> CStream:
        if sid is None:
            sid, self.next_id = self.next_id, self.next_id + 2
        st = self.streams[sid] = CStream(self, sid, auto_window, syn_on_data)
        if not syn_on_data:
            await self.send(WINUP, SYN, sid, 0)
        return st

    async def socks(self, host: str, port: int, atyp: int = 1, expect_rep: int = 0, **kw) -> CStream:
        """Open a stream, send the SOCKS request and check the reply."""
        st = await self.open(**kw)
        await st.write(socks_request(host, port, atyp))
        rep = await within(st.read(10), 20, "SOCKS reply")
        expect(rep == socks_reply(expect_rep), f"SOCKS reply {rep.hex()} != {socks_reply(expect_rep).hex()}")
        return st

    async def ping(self, value: int) -> int:
        await self.send(PING, SYN, 0, value)
        return await within(self.pongs.get(), 5, "Ping ACK")

    async def close(self) -> None:
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)
        self.w.close()
        with contextlib.suppress(Exception):
            await self.w.wait_closed()

    def _bad(self, msg: str) -> None:
        self.violations.append(msg)

    async def _loop(self) -> None:
        try:
            while True:
                ver, typ, flags, sid, length = HDR.unpack(await self.r.readexactly(12))
                payload = await self.r.readexactly(length) if typ == DATA and length else b""
                if ver != 0:
                    self._bad(f"server sent version {ver}")
                if typ == PING:
                    if flags & SYN:
                        self.server_pings.append(length)
                        if self.answer_pings:
                            self.send_nowait(PING, ACK, 0, length)
                    elif flags & ACK:
                        self.pongs.put_nowait(length)
                    else:
                        self._bad("Ping without SYN or ACK")
                elif typ == GOAWAY:
                    self.goaway = length
                elif typ in (DATA, WINUP):
                    self._on_stream_frame(typ, flags, sid, length, payload)
                else:
                    self._bad(f"server sent unknown type {typ}")
        except (asyncio.IncompleteReadError, OSError):
            pass
        finally:
            self.closed.set()
            for st in self.streams.values():
                st.rx.set()
                st.tx.set()

    def _on_stream_frame(self, typ, flags, sid, length, payload) -> None:
        st = self.streams.get(sid)
        if st is None:
            return self._bad(f"frame for stream {sid} which this client never opened")
        if flags & SYN:
            self._bad(f"stream {sid}: server sent SYN")
        if flags & ACK:
            if st.acked or typ != WINUP or length != 0:
                self._bad(f"stream {sid}: bad ACK (type={typ} len={length} dup={st.acked})")
            st.acked = True
        if typ == DATA:
            if st.fin:
                self._bad(f"stream {sid}: Data after FIN")
            if length > MAX_FRAME:
                self._bad(f"stream {sid}: Data frame of {length} bytes > 32 KiB")
            if length > st.recv_window:
                self._bad(f"stream {sid}: server exceeded our window ({length} > {st.recv_window})")
            st.recv_window -= length
            st.received += length
            st.buf += payload
            if st.auto_window:
                st.unacked += length
                if st.unacked >= WINDOW // 2:
                    delta, st.unacked = st.unacked, 0
                    st.grant(delta)
        elif length:
            st.send_window += length
            st.returned += length
            if st.returned > st.sent:
                self._bad(f"stream {sid}: server returned more window ({st.returned}) than sent ({st.sent})")
            st.tx.set()
        if flags & FIN:
            if typ != WINUP or length != 0:
                self._bad(f"stream {sid}: FIN must be WindowUpdate|FIN with delta 0")
            st.fin = True
        if flags & RST:
            st.rst = True
            st.tx.set()
        st.rx.set()


# ---------------------------------------------------------------------------------
# Local destination servers
# ---------------------------------------------------------------------------------

class Dests:
    REPLY = bytes(range(256)) * 1200          # ~300 KB reply of the half-close server

    async def start(self) -> None:
        self.servers = []
        self.writers = set()
        self.open = 0                         # live destination-side connections
        self.source_open = 0
        self.release = asyncio.Event()        # lets the "slow sink" start reading
        self.echo_port = await self._listen(self._echo)
        self.halfclose_port = await self._listen(self._read_all_then_reply)
        self.source_port = await self._listen(self._source)
        self.sink_port = await self._listen(self._slow_sink)
        self.reset_port = await self._listen(self._reset)
        try:
            self.echo6_port = await self._listen(self._echo, "::1")
        except OSError:
            self.echo6_port = None

    async def reset(self) -> None:
        """After a failed check: drop leftovers so they cannot fail (or hang) what follows."""
        self.release.set()
        for w in list(self.writers):
            w.transport.abort()
        with contextlib.suppress(AssertionError):
            await until(lambda: self.open == 0, 2)
        self.release.clear()

    async def stop(self) -> None:
        await self.reset()
        for s in self.servers:
            s.close()
            await s.wait_closed()

    async def _listen(self, handler, host: str = "127.0.0.1") -> int:
        async def run(reader, writer):
            self.open += 1
            self.writers.add(writer)
            try:
                await handler(reader, writer)
            except (OSError, asyncio.IncompleteReadError):
                pass
            finally:
                self.open -= 1
                self.writers.discard(writer)
                writer.close()
                with contextlib.suppress(Exception):
                    await writer.wait_closed()
        s = await asyncio.start_server(run, host, 0)
        self.servers.append(s)
        return s.sockets[0].getsockname()[1]

    async def _echo(self, r, w) -> None:
        while True:
            data = await r.read(65536)
            if not data:
                return
            w.write(data)
            await w.drain()

    async def _read_all_then_reply(self, r, w) -> None:
        """Answers only after EOF: sha256 of everything received + a fixed blob."""
        h = hashlib.sha256()
        while True:
            data = await r.read(65536)
            if not data:
                break
            h.update(data)
        w.write(h.digest() + self.REPLY)
        await w.drain()

    async def _source(self, r, w) -> None:
        """Sends forever, until the connection breaks."""
        self.source_open += 1
        try:
            chunk = b"S" * 65536
            while True:
                w.write(chunk)
                await w.drain()
        finally:
            self.source_open -= 1

    async def _slow_sink(self, r, w) -> None:
        """Reads nothing until released, then reads to EOF and reports the byte count."""
        await self.release.wait()
        total = 0
        while True:
            data = await r.read(1 << 20)
            if not data:
                break
            total += len(data)
        w.write(struct.pack(">Q", total))
        await w.drain()

    async def _reset(self, r, w) -> None:
        """Reads one byte, then resets the connection (SO_LINGER 0)."""
        await r.read(1)
        w.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        w.transport.abort()


# ---------------------------------------------------------------------------------
# Test environment (one per transport: plain / tls)
# ---------------------------------------------------------------------------------

class Env:
    def __init__(self, label: str, dests: Dests, cert: str = "", key: str = ""):
        self.label, self.d = label, dests
        self.cert, self.key = cert, key
        self.server_ctx = self.client_ctx = None
        if cert:
            self.server_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.server_ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            self.server_ctx.load_cert_chain(cert, key)
            self.client_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)   # verifies cert + hostname
            self.client_ctx.load_verify_locations(cert)

    async def start(self) -> None:
        self.server = srv.YamuxServer("127.0.0.1", 0, self.server_ctx)
        await self.server.start()

    async def stop(self) -> None:
        await self.server.stop()

    @contextlib.asynccontextmanager
    async def session(self, server=None):
        c = await YClient.connect((server or self.server).port, self.client_ctx)
        try:
            yield c
        finally:
            await c.close()
        expect(not c.violations, "protocol violations by server: " + "; ".join(c.violations[:3]))

    def streams(self) -> int:
        return sum(len(s.streams) for s in self.server.sessions)

    async def idle(self) -> None:
        """After a check: no session, no stream task, no destination connection left."""
        await until(lambda: not self.server.sessions, 5, "server sessions to close")
        await until(lambda: self.d.open == 0, 5, "destination connections to close")


# ---------------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------------

async def echo_roundtrip(st: CStream, size: int = 1000) -> None:
    msg = os.urandom(size)
    await st.write(msg)
    got = await within(st.read(size), 20, "echo")
    expect(got == msg, f"stream {st.id}: echoed data differs")


async def finish(st: CStream) -> None:
    await st.send_fin()
    await within(st.wait_fin(), 10, "FIN from server")


async def check_echo(env: Env) -> None:
    async with env.session() as c:
        for atyp, host in ((1, "127.0.0.1"), (3, "localhost")):
            st = await c.socks(host, env.d.echo_port, atyp)
            expect(st.acked, "SYN was not answered with WindowUpdate|ACK")
            await echo_roundtrip(st)
            await finish(st)
        # Variant: SYN on a Data frame, request and application bytes in the same frame.
        st = await c.open(syn_on_data=True)
        await st.write(socks_request("localhost", env.d.echo_port, 3) + b"early bytes")
        expect(await within(st.read(10)) == socks_reply(0), "bad reply (SYN on Data)")
        expect(await within(st.read(11)) == b"early bytes", "bytes after the request were lost")
        await finish(st)
        await until(lambda: env.streams() == 0, 3, "finished streams to be removed")


async def check_big_transfer(env: Env) -> None:
    data = os.urandom(20 * MB)
    async with env.session() as c:
        st = await c.socks("127.0.0.1", env.d.echo_port)

        async def pump():
            await st.write(data)
            await st.send_fin()
        writer = asyncio.create_task(pump())
        try:
            h, n = hashlib.sha256(), 0
            while True:
                chunk = await within(st.read_some(), 30, "echo data")
                if not chunk:
                    break
                h.update(chunk)
                n += len(chunk)
            await writer
        finally:
            writer.cancel()
        expect(n == len(data), f"received {n} of {len(data)} bytes")
        expect(h.digest() == hashlib.sha256(data).digest(), "sha256 mismatch")


async def check_concurrent(env: Env) -> None:
    async with env.session() as c:
        async def one():
            st = await c.socks("127.0.0.1", env.d.echo_port)
            data = os.urandom(200 * KB)
            writer = asyncio.create_task(st.write(data))
            try:
                got = await within(st.read(len(data)), 60, "200 KB echo")
                await writer
            finally:
                writer.cancel()
            expect(got == data, f"stream {st.id}: data differs")
            await finish(st)
        await asyncio.gather(*[one() for _ in range(50)])
        await until(lambda: env.streams() == 0, 3, "all 50 streams to be removed")


async def check_refused(env: Env) -> None:
    async with env.session() as c:
        st = await c.socks("127.0.0.1", free_port(), expect_rep=5)
        await within(st.wait_fin(), 5, "FIN after the failure reply")
        expect(env.streams() == 1, "stream must stay until the client FINs")
        await st.send_fin()
        await until(lambda: env.streams() == 0, 3, "stream removal after client FIN")
        expect(not st.rst, "server must FIN, not RST, after a failure reply")


async def check_half_close(env: Env) -> None:
    data = os.urandom(500 * KB)
    async with env.session() as c:
        st = await c.socks("127.0.0.1", env.d.halfclose_port)
        await st.write(data)
        await st.send_fin()
        got = await within(st.read_to_fin(), 20, "reply after half-close")
        expect(got[:32] == hashlib.sha256(data).digest(), "destination did not see all data before EOF")
        expect(got[32:] == env.d.REPLY, f"reply truncated: {len(got) - 32} of {len(env.d.REPLY)} bytes")
        await until(lambda: env.streams() == 0, 3, "stream removal after both FINs")


async def check_rst(env: Env) -> None:
    async with env.session() as c:
        other = await c.socks("127.0.0.1", env.d.echo_port)
        st = await c.socks("127.0.0.1", env.d.source_port)
        await within(st._wait(lambda: st.received >= 1 * MB), 20, "1 MB from the source")
        expect(env.d.source_open == 1, "source connection should be open")
        await st.send_rst()
        await until(lambda: env.d.source_open == 0, 3, "destination connection to be aborted")
        await until(lambda: env.streams() == 1, 3, "reset stream to be removed")
        await echo_roundtrip(other, 100 * KB)          # the old stream still works
        fresh = await c.socks("127.0.0.1", env.d.echo_port)
        await echo_roundtrip(fresh)                    # and new ones can be opened
        await finish(other)
        await finish(fresh)


async def check_split_request(env: Env) -> None:
    async with env.session() as c:
        st = await c.open()
        for byte in socks_request("localhost", env.d.echo_port, 3):
            await st.write(bytes([byte]))
        expect(await within(st.read(10)) == socks_reply(0), "bad reply to a split request")
        await echo_roundtrip(st)
        await finish(st)


async def check_ping(env: Env) -> None:
    async with env.session() as c:
        for value in (0xDEADBEEF, 0, 0xFFFFFFFF):
            got = await c.ping(value)
            expect(got == value, f"Ping ACK value {got:#x} != {value:#x}")


async def check_full_window_frame(env: Env) -> None:
    async with env.session() as c:
        st = await c.open()
        req = socks_request("127.0.0.1", env.d.echo_port)
        # One Data frame that fills the whole initial window: request + payload.
        body = os.urandom(WINDOW - len(req))
        st.send_window -= WINDOW
        st.sent += WINDOW
        await c.send(DATA, 0, st.id, WINDOW, req + body)
        expect(await within(st.read(10)) == socks_reply(0), "bad reply")
        expect(await within(st.read(len(body)), 20, "echo") == body, "echo of the 256 KiB frame differs")
        # All of it was consumed, so the whole window must come back; do it again.
        await within(st._wait(lambda: st.send_window == WINDOW), 5, "window to be returned in full")
        body = os.urandom(WINDOW)
        st.send_window -= WINDOW
        st.sent += WINDOW
        await c.send(DATA, 0, st.id, WINDOW, body)
        expect(await within(st.read(WINDOW), 20, "echo") == body, "echo of the second frame differs")
        await finish(st)


async def check_ipv6(env: Env) -> None:
    if env.d.echo6_port is None:
        raise Skip("no IPv6 loopback on this machine")
    async with env.session() as c:
        st = await c.socks("::1", env.d.echo6_port, 4)
        await echo_roundtrip(st)
        await finish(st)


async def check_socks_errors(env: Env) -> None:
    port = env.d.echo_port
    cases = (("CMD 02 (BIND)", socks_request("127.0.0.1", port, cmd=2), 7),
             ("ATYP 05", socks_request("127.0.0.1", port, atyp=5), 8),
             ("VER 04", socks_request("127.0.0.1", port, ver=4), 1))
    async with env.session() as c:
        for name, req, rep in cases:
            st = await c.open()
            await st.write(req)
            got = await within(st.read(10), 5, f"reply for {name}")
            expect(got == socks_reply(rep), f"{name}: reply {got.hex()}, expected REP {rep:02d}")
            await within(st.wait_fin(), 5, f"FIN after {name}")
            await st.send_fin()
        st = await c.open()                            # FIN in the middle of the request
        await st.write(b"\x05\x01\x00")
        await st.send_fin()
        expect(await within(st.read(10)) == socks_reply(1), "truncated request: expected REP 01")
        await within(st.wait_fin(), 5, "FIN after truncated request")
        await until(lambda: env.streams() == 0, 3, "failed streams to be removed")


async def check_unknown_stream(env: Env) -> None:
    async with env.session() as c:
        await c.send(DATA, 0, 9999, 5000, os.urandom(5000))
        await c.send(WINUP, 0, 7777, 1234)
        await c.send(DATA, FIN, 5555, 3, b"abc")
        await c.send(WINUP, RST, 3333, 0)
        expect(await c.ping(42) == 42, "ping after frames for unknown streams")
        st = await c.socks("127.0.0.1", env.d.echo_port)   # framing is still in sync
        await echo_roundtrip(st)
        await finish(st)
        expect(c.goaway is None and not c.closed.is_set(), "session must stay open")


async def check_protocol_errors(env: Env) -> None:
    async def dup_syn(c):
        st = await c.open()
        await c.send(WINUP, SYN, st.id, 0)

    async def even_id(c):
        await c.send(WINUP, SYN, 2, 0)

    async def bad_version(c):
        await c.send(PING, SYN, 0, 1, ver=1)

    async def bad_type(c):
        await c.send(7, 0, 0, 0)

    async def window_overflow(c):
        st = await c.open()
        await c.send(DATA, 0, st.id, WINDOW + 1, os.urandom(WINDOW + 1))

    for case in (dup_syn, even_id, bad_version, bad_type, window_overflow):
        c = await YClient.connect(env.server.port, env.client_ctx)
        try:
            with contextlib.suppress(OSError):         # server may close while we still write
                await case(c)
            await within(c.closed.wait(), 5, f"{case.__name__}: session close")
            expect(c.goaway == 1, f"{case.__name__}: expected GoAway(1), got {c.goaway}")
        finally:
            await c.close()


async def check_flow_control(env: Env) -> None:
    async with env.session() as c:
        # Server -> client: without window updates the server must stop at 256 KiB.
        st = await c.socks("127.0.0.1", env.d.source_port, auto_window=False)
        # (st.received counts every payload byte, including the 10-byte SOCKS reply.)
        await within(st._wait(lambda: st.received >= WINDOW), 10, "first window of data")
        await asyncio.sleep(0.3)
        expect(st.received == WINDOW, f"server sent {st.received} bytes into a 256 KiB window")
        st.grant(WINDOW)
        await within(st._wait(lambda: st.received >= 2 * WINDOW), 10, "data after WindowUpdate")
        await asyncio.sleep(0.2)
        expect(st.received == 2 * WINDOW, "server overran the second window")
        await st.send_rst()

        # Client -> server: the destination does not read, so window must stop coming back.
        total = 32 * MB
        st = await c.socks("127.0.0.1", env.d.sink_port)
        writer = asyncio.create_task(st.write(bytes(total)))
        try:
            last, stable_since = -1, time.monotonic()
            while time.monotonic() - stable_since < 0.7:
                if st.sent != last:
                    last, stable_since = st.sent, time.monotonic()
                await asyncio.sleep(0.05)
            expect(st.sent < total, "upload never stalled although the destination reads nothing")
            stalled_at = st.sent
            env.d.release.set()                        # destination starts reading
            await within(writer, 60, "upload to finish after the destination resumed")
            await st.send_fin()
            got = await within(st.read_to_fin(), 20, "byte count from the sink")
            expect(struct.unpack(">Q", got)[0] == total, "sink received a different byte count")
        finally:
            writer.cancel()
            env.d.release.clear()
        check_flow_control.note = f"upload stalled at {stalled_at / MB:.1f} MB until the destination read"


async def check_dest_reset(env: Env) -> None:
    async with env.session() as c:
        st = await c.socks("127.0.0.1", env.d.reset_port)
        await st.write(b"x")
        await within(st.wait_rst(), 5, "RST after destination reset")
        await until(lambda: env.streams() == 0, 3, "reset stream to be removed")


async def check_tunnel_drop(env: Env) -> None:
    c = await YClient.connect(env.server.port, env.client_ctx)
    try:
        for _ in range(3):
            st = await c.socks("127.0.0.1", env.d.source_port, auto_window=False)
        expect(env.d.source_open == 3, "three destination connections expected")
        c.w.transport.abort()                          # tunnel dies without any GoAway/FIN
        await until(lambda: env.d.source_open == 0, 3, "destination connections to be aborted")
    finally:
        await c.close()


async def check_goaway(env: Env) -> None:
    async with env.session() as c:
        st = await c.socks("127.0.0.1", env.d.echo_port)
        await c.send(GOAWAY, 0, 0, 0)
        await echo_roundtrip(st, 300 * KB)             # existing stream keeps working
        late = await c.open()                          # new stream is refused
        await within(late.wait_rst(), 5, "RST for a SYN after GoAway(0)")
        await echo_roundtrip(st)
        await finish(st)
        expect(not c.closed.is_set(), "session must stay open after GoAway(0)")
    c = await YClient.connect(env.server.port, env.client_ctx)
    try:
        await c.send(GOAWAY, 0, 0, 2)                  # non-zero code: server closes
        await within(c.closed.wait(), 5, "session close after GoAway(2)")
    finally:
        await c.close()


async def check_keepalive(env: Env) -> None:
    ka = srv.YamuxServer("127.0.0.1", 0, env.server_ctx, keepalive=0.2)
    saved, srv.PING_TIMEOUT = srv.PING_TIMEOUT, 0.6
    await ka.start()
    try:
        async with env.session(ka) as c:               # answers pings: stays alive
            await asyncio.sleep(1.3)
            expect(len(c.server_pings) >= 3, f"expected >= 3 keepalive pings, got {len(c.server_pings)}")
            expect(not c.closed.is_set(), "session closed although pings were answered")
            expect(await c.ping(7) == 7, "client ping during keepalive")
        async with env.session(ka) as c:               # ignores pings: must be closed
            c.answer_pings = False
            await within(c.closed.wait(), 3, "session close after missing Ping ACK")
            expect(c.server_pings, "no keepalive ping was sent")
    finally:
        srv.PING_TIMEOUT = saved
        await ka.stop()


async def check_cli(env: Env) -> None:
    """Run server.py as a real process under -X dev and stop it with SIGINT."""
    cmd = [sys.executable, "-X", "dev", os.path.join(HERE, "server.py"), "--listen", "127.0.0.1:0", "-v"]
    if env.cert:
        cmd += ["--tls-cert", env.cert, "--tls-key", env.key]
    proc = await asyncio.create_subprocess_exec(*cmd, stderr=asyncio.subprocess.PIPE)
    try:
        port = None
        while port is None:
            line = (await within(proc.stderr.readline(), 10, "server start-up")).decode()
            expect(line, "server.py exited during start-up")
            if "listening on" in line:
                port = int(line.split("listening on ")[1].split()[0].rsplit(":", 1)[1])
        c = await YClient.connect(port, env.client_ctx)
        try:
            st = await c.socks("127.0.0.1", env.d.echo_port)
            await echo_roundtrip(st)
            proc.send_signal(signal.SIGINT)            # with a session and a stream still open
            await within(c.closed.wait(), 5, "tunnel close on SIGINT")
            expect(c.goaway == 0, f"expected GoAway(0) on shutdown, got {c.goaway}")
        finally:
            await c.close()
        _, err = await within(proc.communicate(), 10, "server.py exit")
        text = err.decode()
        expect(proc.returncode == 0, f"exit code {proc.returncode}\n{text[-2000:]}")
        for bad in ("Traceback", "Warning", "was never", "unclosed", "Task was destroyed"):
            expect(bad not in text, f"server.py stderr contains {bad!r}:\n{text[-2000:]}")
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.communicate()


CHECKS = [
    ("01", "echo round trip via ATYP 01 and ATYP 03 (localhost)", check_echo),
    ("02", "20 MB through echo, sha256 verified", check_big_transfer),
    ("03", "50 concurrent streams x 200 KB", check_concurrent),
    ("04", "connection refused -> REP 05 then FIN", check_refused),
    ("05", "half-close: data + FIN, full reply after EOF, then FIN", check_half_close),
    ("06", "client RST mid-transfer aborts destination, other streams live", check_rst),
    ("07", "SOCKS request split one byte per frame", check_split_request),
    ("08", "Ping SYN -> Ping ACK with the same value", check_ping),
    ("09", "single 256 KiB Data frame accepted", check_full_window_frame),
    # 10 = this whole list again over TLS, see main()
    ("11", "extra: ATYP 04 (IPv6 ::1)", check_ipv6),
    ("12", "extra: SOCKS errors -> REP 07 / 08 / 01 then FIN", check_socks_errors),
    ("13", "extra: frames for unknown streams are discarded", check_unknown_stream),
    ("14", "extra: protocol errors -> GoAway(1) and close", check_protocol_errors),
    ("15", "extra: flow control stalls and resumes in both directions", check_flow_control),
    ("16", "extra: destination reset -> RST on the stream", check_dest_reset),
    ("17", "extra: GoAway(0) keeps streams, refuses new; GoAway(2) closes", check_goaway),
    ("18", "extra: --keepalive pings, closes when unanswered", check_keepalive),
    ("19", "extra: server.py CLI under -X dev, clean exit on SIGINT", check_cli),
    ("20", "extra: tunnel drop aborts all destination connections", check_tunnel_drop),
]


async def run_suite(label: str, dests: Dests, cert: str = "", key: str = "") -> tuple[int, int]:
    env = Env(label, dests, cert, key)
    await env.start()
    passed = failed = 0
    try:
        for num, title, fn in CHECKS:
            t0 = time.monotonic()
            fn.note = ""
            try:
                await asyncio.wait_for(fn(env), CHECK_TIMEOUT)
                await env.idle()
                status, detail = "PASS", fn.note
                passed += 1
            except Skip as exc:
                status, detail = "SKIP", str(exc)
            except Exception as exc:  # noqa: BLE001 - report every failure
                status, detail = "FAIL", f"{type(exc).__name__}: {exc}"
                failed += 1
                traceback.print_exc()
                for session in list(env.server.sessions):      # do not let leftovers cascade
                    session.writer.transport.abort()
                await dests.reset()
            took = time.monotonic() - t0
            print(f"{status} [{label}] {num} {title} ({took:.2f}s)" + (f" -- {detail}" if detail else ""),
                  flush=True)
    finally:
        await env.stop()
    return passed, failed


def make_cert(directory: str) -> tuple[str, str]:
    cert, key, cfg = (os.path.join(directory, n) for n in ("cert.pem", "key.pem", "openssl.cnf"))
    with open(cfg, "w") as f:
        f.write("[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n"
                "[dn]\nCN=localhost\n"
                "[v3]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
                    "-config", cfg, "-keyout", key, "-out", cert],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


async def amain() -> int:
    asyncio.get_running_loop().slow_callback_duration = 1.0   # -X dev: hashing 20 MB is not a bug
    dests = Dests()
    await dests.start()
    failed = 0
    try:
        passed, failed = await run_suite("plain", dests)
        if shutil.which("openssl") is None:
            print("SKIP [tls] 10 same suite over TLS -- openssl CLI not found, TLS checks were NOT run")
        else:
            with tempfile.TemporaryDirectory() as tmp:
                cert, key = make_cert(tmp)
                tls_passed, tls_failed = await run_suite("tls", dests, cert, key)
            print(f"{'FAIL' if tls_failed else 'PASS'} [tls] 10 same suite over TLS "
                  f"({tls_passed} passed, {tls_failed} failed)")
            failed += tls_failed
    finally:
        await dests.stop()
    print(f"\nRESULT: {'ALL CHECKS PASSED' if not failed else f'{failed} CHECK(S) FAILED'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(amain()))
