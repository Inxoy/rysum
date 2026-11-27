#!/usr/bin/env python3
"""Rysum Konzert relay: lets a player and listeners meet through a server on the internet.

Neither side needs an open port: the player connects out to the relay and claims a room; every
listener connects out too and asks for that room. For each listener the relay asks the player for a
fresh connection and then joins the two byte for byte. Everything else - the password check, the
music - happens between player and listener; the relay only passes bytes on.

    python3 rysum_relay.py                   # port 8790 on every address
    python3 rysum_relay.py --port 9000
    python3 rysum_relay.py --host 127.0.0.1  # only this computer (tests)

Players type  server[:port]/room  under "Relay" in the Konzert window, listeners the same text as
the address. Open the port (TCP) in the server's firewall. Python 3.8 or newer, nothing to install.

The protocol (one line each, then raw bytes):
  player   -> "RYSUM-RELAY 1 HOST <room>"           <- "OK" | "ERR taken"; later "L <id>" per listener,
                                                       "P" now and then (the player says "P" too)
  listener -> "RYSUM-RELAY 1 JOIN <room>"           <- "OK" once joined to the player | "ERR noroom"
  player   -> "RYSUM-RELAY 1 ATTACH <room> <id>"    <- "OK" | "ERR gone", then joined to listener <id>
"""

import argparse
import asyncio
import logging
import os
import re
import time

ROOM = re.compile(r"^[a-z0-9_-]{1,32}$")
MAX_ROOMS = 200
MAX_WAITING = 32          # listeners waiting for the player's line, per room
MAX_PAIRS = 64            # listeners joined, per room
WAIT_FOR_ATTACH = 15.0    # s a listener waits for the player's line
IDLE_HOST = 60.0          # s without a word from the player: the room closes

log = logging.getLogger("rysum-relay")


class Room:
    def __init__(self, name, writer):
        self.name = name
        self.writer = writer               # the player's control line
        self.waiting = {}                  # id -> (reader, writer, future)
        self.pairs = 0
        self.next_id = 1
        self.seen = time.monotonic()


rooms = {}


async def read_line(reader, timeout):
    line = await asyncio.wait_for(reader.readline(), timeout)
    if not line or len(line) > 200:
        raise ConnectionError("no line")
    return line.decode("utf-8", "replace").strip()


async def send(writer, text):
    writer.write((text + "\n").encode())
    await writer.drain()


def close(writer):
    try:
        writer.close()
    except Exception:
        pass


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except Exception:
        pass
    finally:
        close(writer)


async def splice(a_reader, a_writer, b_reader, b_writer):
    await asyncio.gather(pipe(a_reader, b_writer), pipe(b_reader, a_writer))


async def host(name, reader, writer):
    if name in rooms:
        await send(writer, "ERR taken")
        return
    if len(rooms) >= MAX_ROOMS:
        await send(writer, "ERR full")
        return
    room = Room(name, writer)
    rooms[name] = room
    log.info("room %s opened", name)
    await send(writer, "OK")
    try:
        while True:
            try:
                line = await read_line(reader, 20.0)
            except asyncio.TimeoutError:
                if time.monotonic() - room.seen > IDLE_HOST:
                    break
                await send(writer, "P")
                continue
            room.seen = time.monotonic()
            if line != "P":
                break
    except Exception:
        pass
    finally:
        if rooms.get(name) is room:
            del rooms[name]
        for _, (_, w, fut) in list(room.waiting.items()):
            if not fut.done():
                fut.set_result(None)
        log.info("room %s closed", name)


async def join(name, reader, writer):
    room = rooms.get(name)
    if room is None:
        await send(writer, "ERR noroom")
        return
    if len(room.waiting) >= MAX_WAITING or room.pairs >= MAX_PAIRS:
        await send(writer, "ERR full")
        return
    lid = room.next_id
    room.next_id += 1
    fut = asyncio.get_running_loop().create_future()
    room.waiting[lid] = (reader, writer, fut)
    try:
        await send(room.writer, "L %d" % lid)
        player = await asyncio.wait_for(fut, WAIT_FOR_ATTACH)
    except Exception:
        player = None
    finally:
        room.waiting.pop(lid, None)
    if player is None:
        await send(writer, "ERR noroom")
        return
    p_reader, p_writer = player
    room.pairs += 1
    log.info("room %s: listener %d joined (%d)", name, lid, room.pairs)
    try:
        await send(writer, "OK")
        await splice(reader, writer, p_reader, p_writer)
    finally:
        room.pairs -= 1
        close(p_writer)


async def attach(name, lid, reader, writer):
    room = rooms.get(name)
    entry = room.waiting.get(lid) if room is not None else None
    if entry is None or entry[2].done():
        await send(writer, "ERR gone")
        return False
    await send(writer, "OK")
    entry[2].set_result((reader, writer))
    return True


async def handle(reader, writer):
    keep = False
    try:
        words = (await read_line(reader, 10.0)).split()
        if len(words) < 4 or words[0] != "RYSUM-RELAY" or words[1] != "1" or not ROOM.match(words[3]):
            await send(writer, "ERR what")
            return
        what, name = words[2], words[3]
        if what == "HOST" and len(words) == 4:
            await host(name, reader, writer)
        elif what == "JOIN" and len(words) == 4:
            await join(name, reader, writer)
        elif what == "ATTACH" and len(words) == 5 and words[4].isdigit():
            # (the player's line now belongs to the listener's pair: it is closed there)
            keep = await attach(name, int(words[4]), reader, writer)
        else:
            await send(writer, "ERR what")
    except Exception:
        pass
    finally:
        if not keep:
            close(writer)


async def main():
    ap = argparse.ArgumentParser(description="Rysum Konzert relay")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--port", type=int, default=8790, help="TCP port (default 8790)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--lifetime", type=float, default=0.0, help="stop after so many seconds (tests; 0 = never)")
    args = ap.parse_args()
    if args.lifetime > 0:
        asyncio.get_running_loop().call_later(args.lifetime, lambda: os._exit(0))
    logging.basicConfig(level=logging.WARNING if args.quiet else logging.INFO,
                        format="%(asctime)s %(message)s")
    server = await asyncio.start_server(handle, args.host, args.port)
    log.info("Rysum relay on %s:%d", args.host, args.port)
    print("ready", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
