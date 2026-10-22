#!/usr/bin/env python3
"""TCP relay that re-fragments the byte stream: random chunk sizes (1..max), random
small delays and random coalescing, in both directions. Bytes are never lost or
reordered. Put it between muxstress client and server so every Yamux frame arrives
split across reads in every possible way.

  python3 chaos_relay.py --listen 127.0.0.1:9301 --target 127.0.0.1:9300 [--max-chunk 700] [--delay-ms 3]
"""
import argparse, asyncio, random

async def pump(reader, writer, rng, max_chunk, delay_ms, name):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            pos = 0
            while pos < len(data):
                mode = rng.random()
                if mode < 0.3:
                    n = 1 + rng.randrange(min(4, max_chunk))          # byte-by-byte-ish
                elif mode < 0.6:
                    n = 1 + rng.randrange(max_chunk)                  # random
                elif mode < 0.7:
                    n = 12 + rng.randrange(-3, 4)                     # around the yamux header size
                else:
                    n = len(data) - pos                               # the rest at once
                n = max(1, min(n, len(data) - pos))
                writer.write(data[pos:pos + n])
                pos += n
                if rng.random() < 0.2:
                    await writer.drain()
                    await asyncio.sleep(rng.random() * delay_ms / 1000.0)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            writer.write_eof()
        except Exception:
            pass

async def handle(client_r, client_w, args):
    try:
        srv_r, srv_w = await asyncio.open_connection(*args.target)
    except OSError as e:
        client_w.close()
        return
    rng = random.Random()
    t1 = asyncio.create_task(pump(client_r, srv_w, rng, args.max_chunk, args.delay_ms, "c->s"))
    t2 = asyncio.create_task(pump(srv_r, client_w, rng, args.max_chunk, args.delay_ms, "s->c"))
    await asyncio.gather(t1, t2)
    for w in (client_w, srv_w):
        try:
            w.close()
        except Exception:
            pass

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:9301")
    ap.add_argument("--target", default="127.0.0.1:9300")
    ap.add_argument("--max-chunk", type=int, default=700)
    ap.add_argument("--delay-ms", type=float, default=3)
    args = ap.parse_args()
    lh, lp = args.listen.rsplit(":", 1)
    th, tp = args.target.rsplit(":", 1)
    args.target = (th, int(tp))
    server = await asyncio.start_server(lambda r, w: handle(r, w, args), lh, int(lp), limit=1 << 20)
    print("chaos relay %s -> %s:%d (max chunk %d)" % (args.listen, th, int(tp), args.max_chunk), flush=True)
    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
