#!/usr/bin/env python3
"""Yamux (hashicorp spec, version 0) test server with SOCKS5 CONNECT inside each stream.

The client connects over plain TCP or TLS and opens one yamux stream per proxied
connection. On every stream it sends a raw SOCKS5 CONNECT request (no method
negotiation); the server connects to the target, answers with a 10-byte SOCKS5
reply on the stream and then relays bytes in both directions.

Pure standard library, Python 3.9+.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import errno
import logging
import signal
import socket
import ssl
import struct
from typing import Dict, Optional, Set, Tuple

log = logging.getLogger("yamux")

HDR = struct.Struct(">BBHII")  # version, type, flags, stream id, length
TYPE_DATA, TYPE_WINDOW, TYPE_PING, TYPE_GOAWAY = 0, 1, 2, 3
SYN, ACK, FIN, RST = 1, 2, 4, 8
GOAWAY_NORMAL, GOAWAY_PROTOCOL, GOAWAY_INTERNAL = 0, 1, 2

WINDOW = 256 * 1024       # initial window per stream, per direction
MAX_FRAME = 32 * 1024     # largest Data payload we send
CONNECT_TIMEOUT = 15.0    # destination connect (DNS included)
PING_TIMEOUT = 10.0       # keepalive: max wait for Ping|ACK
LINGER = 0.5              # after GoAway(error): keep reading this long before closing

REP_OK, REP_FAILURE, REP_UNREACHABLE, REP_REFUSED, REP_BAD_CMD, REP_BAD_ATYP = 0, 1, 4, 5, 7, 8
UNREACHABLE = {getattr(errno, n, None) for n in
               ("ENETUNREACH", "EHOSTUNREACH", "ETIMEDOUT", "ENETDOWN", "EHOSTDOWN")}


class ProtocolError(Exception):
    """Peer violated yamux: answer GoAway(1) and close the session."""


class SessionClosed(Exception):
    """The tunnel connection is gone; nothing more can be sent."""


class SocksError(Exception):
    """The SOCKS request failed; `rep` is the reply code to send."""

    def __init__(self, rep: int, why: str):
        super().__init__(why)
        self.rep = rep


def socks_reply(rep: int) -> bytes:
    return bytes((5, rep, 0, 1, 0, 0, 0, 0, 0, 0))


def set_nodelay(writer: asyncio.StreamWriter) -> None:
    sock = writer.get_extra_info("socket")
    if sock is not None:
        with contextlib.suppress(OSError):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


async def close_writer(writer: asyncio.StreamWriter, abort: bool = False) -> None:
    """Close (flush) or abort a connection and wait until the socket is really gone."""
    if abort:
        writer.transport.abort()
    else:
        writer.close()
    # A peer that never reads must not keep a graceful close pending forever.
    timer = asyncio.get_running_loop().call_later(2.0, writer.transport.abort)
    try:
        await writer.wait_closed()
    except OSError:  # includes ssl.SSLError and ConnectionError
        pass
    finally:
        timer.cancel()
        writer.transport.abort()   # no-op if closed; matters when we got cancelled


async def open_destination(host: str, port: int) -> Tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    """Resolve `host` and try every address in turn. Raises ConnectionRefusedError
    if any address refused, otherwise the first error."""
    loop = asyncio.get_running_loop()
    infos = await loop.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    error: Optional[OSError] = None
    for family, kind, proto, _, addr in infos:
        sock = socket.socket(family, kind, proto)
        try:
            sock.setblocking(False)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            await loop.sock_connect(sock, addr)
            return await asyncio.open_connection(sock=sock)
        except OSError as exc:
            sock.close()
            if error is None or isinstance(exc, ConnectionRefusedError):
                error = exc
        except BaseException:  # cancelled (client RST / session closed) mid-connect
            sock.close()
            raise
    raise error or OSError(errno.EHOSTUNREACH, "no usable address")


def classify_connect_error(exc: BaseException) -> Tuple[int, str]:
    if isinstance(exc, (asyncio.TimeoutError, TimeoutError)):
        return REP_UNREACHABLE, "timeout"
    if isinstance(exc, (socket.gaierror, UnicodeError)):
        return REP_UNREACHABLE, "dns failure"
    if isinstance(exc, ConnectionRefusedError):
        return REP_REFUSED, "refused"
    if isinstance(exc, OSError) and exc.errno in UNREACHABLE:
        return REP_UNREACHABLE, "unreachable"
    return REP_FAILURE, f"failed: {exc}"


class Stream:
    """State of one client-opened yamux stream."""

    def __init__(self, sid: int):
        self.id = sid
        self.send_window = WINDOW   # bytes we may still send to the client
        self.recv_window = WINDOW   # bytes the client may still send to us
        self.unacked = 0            # consumed bytes not yet returned as window
        self.inbuf = bytearray()    # received from the client, not yet consumed
        self.remote_fin = False     # client sent FIN: no more data from it
        self.discard = False        # SOCKS failed: drop whatever the client still sends
        self.rx_event = asyncio.Event()   # inbuf / remote_fin changed
        self.tx_event = asyncio.Event()   # send_window grew
        self.task: Optional[asyncio.Task] = None

    async def _wait_rx(self) -> None:   # wait for more data or FIN from the client
        self.rx_event.clear()
        await self.rx_event.wait()

    async def read_exact(self, n: int) -> bytes:
        """Read n bytes of the SOCKS request; they are consumed by the server itself."""
        while len(self.inbuf) < n:
            if self.remote_fin:
                raise SocksError(REP_FAILURE, "FIN before the request was complete")
            await self._wait_rx()
        data = bytes(self.inbuf[:n])
        del self.inbuf[:n]
        self.unacked += n
        return data

    async def read_some(self) -> bytes:
        """All buffered client bytes; b"" once the client's FIN is reached."""
        while not self.inbuf and not self.remote_fin:
            await self._wait_rx()
        data, self.inbuf = self.inbuf, bytearray()
        return bytes(data)


class Session:
    """One tunnel connection carrying many streams."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                 name: str, keepalive: float = 0.0):
        self.reader, self.writer = reader, writer
        self.name = name
        self.keepalive = keepalive
        self.streams: Dict[int, Stream] = {}
        self.tasks: Set[asyncio.Task] = set()   # stream tasks, incl. ones still closing
        self.wlock = asyncio.Lock()
        self.remote_goaway = False
        self.reason = "peer closed"
        self.stream_count = 0
        self.ping_id = 0
        self.pong = asyncio.Event()

    # ---- sending -----------------------------------------------------------------

    def _write(self, typ: int, flags: int, sid: int, length: int, payload: bytes = b"") -> bool:
        """Queue one frame without waiting. A frame is always one write() call, so
        frames never interleave; header-only frames use this directly and therefore
        never block the read loop on a slow tunnel."""
        if self.writer.transport.is_closing():
            return False
        log.debug("%s tx type=%d flags=%#x sid=%d len=%d", self.name, typ, flags, sid, length)
        self.writer.write(HDR.pack(0, typ, flags, sid, length) + payload)
        return True

    async def _send(self, typ: int, flags: int, sid: int, length: int, payload: bytes = b"") -> None:
        """Send a frame and wait for the tunnel to drain (backpressure for Data).
        The session-wide lock allows one drain() waiter at a time, so all streams
        queue up fairly while the tunnel is congested."""
        async with self.wlock:
            if not self._write(typ, flags, sid, length, payload):
                raise SessionClosed()
            try:
                await self.writer.drain()
            except OSError:
                raise SessionClosed() from None

    async def _send_data(self, st: Stream, data: bytes) -> None:
        """Send bytes on a stream, limited by the peer's window and MAX_FRAME."""
        view = memoryview(data)
        while view:
            while st.send_window <= 0:   # wait for a WindowUpdate from the client
                st.tx_event.clear()
                await st.tx_event.wait()
            n = min(len(view), st.send_window, MAX_FRAME)
            st.send_window -= n
            await self._send(TYPE_DATA, 0, st.id, n, view[:n])
            view = view[n:]

    def _return_window(self, st: Stream) -> None:
        """Give consumed bytes back once they add up to half the window."""
        if st.unacked >= WINDOW // 2 and not st.remote_fin and self.streams.get(st.id) is st:
            delta, st.unacked = st.unacked, 0
            st.recv_window += delta
            self._write(TYPE_WINDOW, 0, st.id, delta)

    # ---- receiving ---------------------------------------------------------------

    async def run(self) -> None:
        log.info("session %s open", self.name)
        keepalive = asyncio.create_task(self._keepalive()) if self.keepalive > 0 else None
        try:
            await self._read_loop()
        except ProtocolError as exc:
            self.reason = f"protocol error: {exc}"
            await self._goaway_and_linger(GOAWAY_PROTOCOL)
        except asyncio.IncompleteReadError:
            pass                                  # tunnel EOF
        except OSError as exc:
            self.reason = f"tunnel error: {exc}"
        except Exception:
            log.exception("session %s internal error", self.name)
            self.reason = "internal error"
            await self._goaway_and_linger(GOAWAY_INTERNAL)
        finally:
            # The tunnel is gone (or going): abort every destination connection.
            tasks = list(self.tasks) + ([keepalive] if keepalive else [])
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            self.streams.clear()
            await close_writer(self.writer)
            log.info("session %s closed (%s), %d streams served",
                     self.name, self.reason, self.stream_count)

    async def _goaway_and_linger(self, code: int) -> None:
        """Send GoAway, then keep swallowing input for a moment: closing a socket
        with unread data makes TCP send a reset that could destroy the GoAway."""
        if not self._write(TYPE_GOAWAY, 0, 0, code):
            return
        with contextlib.suppress(OSError, asyncio.TimeoutError, TimeoutError):
            if self.writer.can_write_eof():
                self.writer.write_eof()
            await asyncio.wait_for(self._skip(1 << 62), LINGER)

    def shutdown(self) -> None:
        """Server is stopping: say GoAway(0) and close the tunnel."""
        self.reason = "server shutdown"
        self._write(TYPE_GOAWAY, 0, 0, GOAWAY_NORMAL)
        self.writer.close()   # flushes, then the read loop sees EOF
        asyncio.get_running_loop().call_later(1.0, self.writer.transport.abort)

    async def _skip(self, n: int) -> None:
        """Read and drop n bytes (stops early at EOF)."""
        while n > 0:
            chunk = await self.reader.read(min(n, 65536))
            if not chunk:
                break
            n -= len(chunk)

    async def _read_loop(self) -> None:
        while True:
            ver, typ, flags, sid, length = HDR.unpack(await self.reader.readexactly(HDR.size))
            log.debug("%s rx type=%d flags=%#x sid=%d len=%d", self.name, typ, flags, sid, length)
            if ver != 0:
                raise ProtocolError(f"unknown version {ver}")
            if typ in (TYPE_DATA, TYPE_WINDOW):
                await self._on_stream_frame(typ, flags, sid, length)
            elif typ == TYPE_PING:
                if flags & SYN:                        # request: echo the value back
                    self._write(TYPE_PING, ACK, 0, length)
                elif flags & ACK and length == self.ping_id:
                    self.pong.set()                    # answer to our keepalive
            elif typ == TYPE_GOAWAY:
                if length != GOAWAY_NORMAL:
                    self.reason = f"peer sent GoAway({length})"
                    return
                self.remote_goaway = True              # existing streams may finish
            else:
                raise ProtocolError(f"unknown frame type {typ}")

    async def _on_stream_frame(self, typ: int, flags: int, sid: int, length: int) -> None:
        st = self.streams.get(sid)
        if flags & SYN:
            if st is not None or sid % 2 == 0:
                raise ProtocolError(f"invalid SYN for stream {sid}")
            if self.remote_goaway:
                # No new streams after GoAway(0): refuse this one with RST. It is
                # not registered, so the rest of its frames are simply discarded.
                self._write(TYPE_WINDOW, RST, sid, 0)
            else:
                st = self._open_stream(sid)

        payload = b""
        if typ == TYPE_DATA and length:
            if st is None:                 # unknown stream: drop the payload silently
                await self._skip(length)
                return
            if length > st.recv_window:
                raise ProtocolError(f"stream {sid}: {length} bytes exceed the receive "
                                    f"window ({st.recv_window})")
            payload = await self.reader.readexactly(length)   # may be a full 256 KiB
            st.recv_window -= length
        if st is None:
            return

        if flags & RST:                    # client abort: kill the destination now
            del self.streams[sid]
            st.task.cancel()
            return
        if typ == TYPE_DATA:
            # Data after the client's own FIN, or after a failed SOCKS request, is dropped.
            if payload and not st.discard and not st.remote_fin:
                st.inbuf += payload
                st.rx_event.set()
        elif length:                       # WindowUpdate: delta for our send window
            st.send_window += length
            st.tx_event.set()
        if flags & FIN:                    # processed after the payload of the same frame
            st.remote_fin = True
            st.rx_event.set()

    def _open_stream(self, sid: int) -> Stream:
        st = Stream(sid)
        self.streams[sid] = st
        self.stream_count += 1
        self._write(TYPE_WINDOW, ACK, sid, 0)
        st.task = asyncio.create_task(self._serve_stream(st))
        self.tasks.add(st.task)
        st.task.add_done_callback(self.tasks.discard)
        return st

    async def _keepalive(self) -> None:
        while True:
            await asyncio.sleep(self.keepalive)
            self.ping_id = (self.ping_id + 1) & 0xFFFFFFFF
            self.pong.clear()
            self._write(TYPE_PING, SYN, 0, self.ping_id)
            try:
                await asyncio.wait_for(self.pong.wait(), PING_TIMEOUT)
            except (asyncio.TimeoutError, TimeoutError):
                self.reason = "keepalive timeout"
                self.writer.transport.abort()          # the read loop then sees EOF
                return

    # ---- one stream: SOCKS5 request, connect, relay --------------------------------

    async def _serve_stream(self, st: Stream) -> None:
        dest: Optional[asyncio.StreamWriter] = None
        clean = False
        try:
            try:
                host, port = await self._read_request(st)
                reader, dest = await self._connect(st, host, port)
            except SocksError as exc:
                # Failure: reply, FIN (never RST), keep the stream until the client's
                # FIN (an RST cancels this task). Its data is dropped meanwhile.
                st.discard = True
                st.inbuf.clear()
                await self._send_data(st, socks_reply(exc.rep))
                self._write(TYPE_WINDOW, FIN, st.id, 0)
                while not st.remote_fin:
                    await st._wait_rx()
                return
            await self._send_data(st, socks_reply(REP_OK))
            await self._relay(st, reader, dest)
            clean = True                   # FIN sent and FIN received
        except SessionClosed:
            pass
        except OSError as exc:             # destination reset / broken pipe
            log.debug("%s stream %d destination error: %s", self.name, st.id, exc)
            if self.streams.get(st.id) is st:
                self._write(TYPE_WINDOW, RST, st.id, 0)
        finally:
            if self.streams.get(st.id) is st:
                del self.streams[st.id]
            if dest is not None:
                await close_writer(dest, abort=not clean)

    async def _read_request(self, st: Stream) -> Tuple[str, int]:
        """Parse `05 01 00 ATYP ADDR PORT`; it may arrive split over many frames."""
        try:
            ver, cmd, _rsv, atyp = await st.read_exact(4)
            if ver != 5:
                raise SocksError(REP_FAILURE, f"bad SOCKS version {ver}")
            if cmd != 1:
                raise SocksError(REP_BAD_CMD, f"unsupported command {cmd}")
            if atyp == 1:
                host = socket.inet_ntoa(await st.read_exact(4))
            elif atyp == 4:
                host = socket.inet_ntop(socket.AF_INET6, await st.read_exact(16))
            elif atyp == 3:
                size = (await st.read_exact(1))[0]
                host = (await st.read_exact(size)).decode("utf-8", "replace")
                if not host:
                    raise SocksError(REP_FAILURE, "empty domain name")
            else:
                raise SocksError(REP_BAD_ATYP, f"unsupported address type {atyp}")
            port = struct.unpack(">H", await st.read_exact(2))[0]
        except SocksError as exc:
            log.info("%s stream %d bad request -> REP %02d (%s)", self.name, st.id, exc.rep, exc)
            raise
        return host, port

    async def _connect(self, st: Stream, host: str, port: int):
        target = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
        try:
            reader, writer = await asyncio.wait_for(open_destination(host, port), CONNECT_TIMEOUT)
        except (OSError, UnicodeError, asyncio.TimeoutError) as exc:
            rep, why = classify_connect_error(exc)
            log.info("%s stream %d connect %s -> REP %02d (%s)", self.name, st.id, target, rep, why)
            raise SocksError(rep, why) from None
        log.info("%s stream %d connect %s -> ok", self.name, st.id, target)
        return reader, writer

    async def _relay(self, st: Stream, reader: asyncio.StreamReader,
                     writer: asyncio.StreamWriter) -> None:
        """Pump both directions; returns when both have seen their FIN."""
        tasks = [asyncio.create_task(self._uplink(st, writer)),
                 asyncio.create_task(self._downlink(st, reader))]
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)
            for task in done:
                task.result()              # re-raise the first failure
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    async def _uplink(self, st: Stream, writer: asyncio.StreamWriter) -> None:
        """client -> destination. Window is returned only after drain()."""
        while True:
            data = await st.read_some()
            if not data:
                break                      # client FIN and everything before it is flushed
            writer.write(data)
            await writer.drain()
            st.unacked += len(data)
            self._return_window(st)
        with contextlib.suppress(OSError):
            if writer.can_write_eof():
                writer.write_eof()         # half-close: the destination may still answer

    async def _downlink(self, st: Stream, reader: asyncio.StreamReader) -> None:
        """destination -> client. Reads only while the stream has send window, so a
        stalled client (or a full tunnel) back-pressures the destination via TCP."""
        while True:
            while st.send_window <= 0:
                st.tx_event.clear()
                await st.tx_event.wait()
            data = await reader.read(min(MAX_FRAME, st.send_window))
            if not data:
                break
            await self._send_data(st, data)
        self._write(TYPE_WINDOW, FIN, st.id, 0)   # destination EOF -> FIN


class YamuxServer:
    def __init__(self, host: str, port: int, ssl_ctx: Optional[ssl.SSLContext] = None,
                 keepalive: float = 0.0):
        self.host, self.port = host, port
        self.ssl_ctx = ssl_ctx
        self.keepalive = keepalive
        self.sessions: Dict[Session, asyncio.Task] = {}
        self.server: Optional[asyncio.AbstractServer] = None

    async def start(self) -> None:
        # limit=WINDOW: the tunnel reader buffers up to one maximum-size Data frame.
        self.server = await asyncio.start_server(self._on_client, self.host, self.port,
                                                 ssl=self.ssl_ctx, limit=WINDOW)
        self.port = self.server.sockets[0].getsockname()[1]
        log.info("listening on %s:%d (%s)", self.host, self.port,
                 "TLS" if self.ssl_ctx else "plain TCP")

    async def _on_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        set_nodelay(writer)
        peer = writer.get_extra_info("peername") or ("?", 0)
        session = Session(reader, writer, f"{peer[0]}:{peer[1]}", self.keepalive)
        self.sessions[session] = asyncio.current_task()
        try:
            await session.run()
        finally:
            del self.sessions[session]

    async def stop(self) -> None:
        if self.server is None:
            return
        self.server.close()
        tasks = list(self.sessions.values())
        for session in list(self.sessions):
            session.shutdown()
        await asyncio.gather(*tasks, return_exceptions=True)
        await self.server.wait_closed()
        self.server = None
        log.info("server stopped")


def parse_listen(value: str) -> Tuple[str, int]:
    host, sep, port = value.rpartition(":")
    if not sep or not port.isdigit():
        raise argparse.ArgumentTypeError("expected HOST:PORT, e.g. 0.0.0.0:9191")
    return host.strip("[]") or "0.0.0.0", int(port)


async def amain(args: argparse.Namespace) -> None:
    ssl_ctx = None
    if args.tls_cert:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ssl_ctx.load_cert_chain(args.tls_cert, args.tls_key)
    server = YamuxServer(args.listen[0], args.listen[1], ssl_ctx, args.keepalive)
    await server.start()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        with contextlib.suppress(NotImplementedError):   # not available on Windows
            loop.add_signal_handler(sig, stop.set)
    try:
        await stop.wait()
    finally:
        await server.stop()


def main() -> None:
    ap = argparse.ArgumentParser(description="Yamux + SOCKS5 CONNECT test server")
    ap.add_argument("--listen", type=parse_listen, default=("0.0.0.0", 9191), metavar="HOST:PORT")
    ap.add_argument("--tls-cert", metavar="cert.pem", help="enable TLS (needs --tls-key)")
    ap.add_argument("--tls-key", metavar="key.pem")
    ap.add_argument("--keepalive", type=float, default=0.0, metavar="SECONDS",
                    help="send Ping every SECONDS, close if no ACK within 10 s (0 = off)")
    ap.add_argument("-v", "--verbose", action="store_true", help="frame-level DEBUG log")
    args = ap.parse_args()
    if bool(args.tls_cert) != bool(args.tls_key):
        ap.error("--tls-cert and --tls-key must be given together")
    logging.basicConfig(level=logging.WARNING, format="%(asctime)s %(levelname)-5s %(message)s")
    log.setLevel(logging.DEBUG if args.verbose else logging.INFO)
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:   # only where add_signal_handler is unavailable
        pass


if __name__ == "__main__":
    main()
