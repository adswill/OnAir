#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""SpyServer TCP protocol tracing proxy. Python 3 standard library only.
Start: python tools/spyserver/trace_proxy.py
Connect one client at a time to 127.0.0.1:5556 instead of server:5555.
"""
import socket
import struct
import threading
import time

LISTEN = ("127.0.0.1", 5556)
UPSTREAM = ("192.168.1.64", 5555)
SETTING_NAMES = {
    0: "STREAMING_MODE", 1: "STREAMING_ENABLED", 2: "GAIN",
    100: "IQ_FORMAT", 101: "IQ_FREQUENCY", 102: "IQ_DECIMATION",
    103: "IQ_DIGITAL_GAIN", 200: "FFT_FORMAT", 201: "FFT_FREQUENCY",
    202: "FFT_DECIMATION", 203: "FFT_DB_OFFSET", 204: "FFT_DB_RANGE",
    205: "FFT_DISPLAY_PIXELS"
}
out_lock = threading.Lock()


def log(message):
    with out_lock:
        print(time.strftime("%H:%M:%S"), message, flush=True)


def pump(src, dst, direction):
    buf = bytearray()
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            # Forward first, then parse a local copy. Never transform bytes.
            dst.sendall(data)
            buf.extend(data)
            if direction == "client":
                while len(buf) >= 8:
                    typ, size = struct.unpack_from("<II", buf)
                    if size > 1024 * 1024:
                        log("client packet invalid size %d" % size)
                        return
                    if len(buf) < 8 + size:
                        break
                    payload = bytes(buf[8:8 + size])
                    del buf[:8 + size]
                    if typ == 0:
                        version = struct.unpack_from("<I", payload)[0] if len(payload) >= 4 else 0
                        name = payload[4:].decode("utf-8", "replace")
                        log("C->S HELLO version=0x%08x name=%r" % (version, name))
                    elif typ == 2 and len(payload) >= 8:
                        setting, value = struct.unpack_from("<II", payload)
                        label = SETTING_NAMES.get(setting, str(setting))
                        mhz = " (%.6f MHz)" % (value / 1e6) if setting in (101, 201) else ""
                        log("C->S SET %s=%d%s" % (label, value, mhz))
                    else:
                        log("C->S command=%d size=%d" % (typ, size))
            else:
                while len(buf) >= 20:
                    proto, typ, stream, seq, size = struct.unpack_from("<IIIII", buf)
                    if size > 1024 * 1024:
                        log("server packet invalid size %d" % size)
                        return
                    if len(buf) < 20 + size:
                        break
                    payload = bytes(buf[20:20 + size])
                    del buf[:20 + size]
                    if typ == 1 and len(payload) >= 36:
                        fields = struct.unpack_from("<9I", payload)
                        log("S->C SYNC control=%d device=%.6f IQ=%.6f FFT=%.6f MHz" %
                            (fields[0], fields[2] / 1e6, fields[3] / 1e6, fields[4] / 1e6))
                    elif typ == 0:
                        log("S->C DEVICE_INFO size=%d" % size)
                    elif typ == 2:
                        log("S->C PONG")
                    # IQ/FFT data is intentionally not logged; no giant dumps.
                    if len(buf) > 4 * 1024 * 1024:
                        log("server parser buffered too much, clearing")
                        buf.clear()
    except (OSError, ConnectionError) as exc:
        log("%s connection ended: %s" % (direction, exc))
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(LISTEN)
        listener.listen(1)
        log("Tracing %s:%s -> %s:%s" % (*LISTEN, *UPSTREAM))
        while True:
            incoming, addr = listener.accept()
            log("Client connected: %s:%s" % addr)
            try:
                outgoing = socket.create_connection(UPSTREAM, timeout=10)
                outgoing.settimeout(None)
            except OSError as exc:
                log("Upstream failed: %s" % exc)
                incoming.close()
                continue
            with incoming, outgoing:
                one = threading.Thread(target=pump, args=(incoming, outgoing, "client"), daemon=True)
                two = threading.Thread(target=pump, args=(outgoing, incoming, "server"), daemon=True)
                one.start()
                two.start()
                one.join()
                two.join()
            log("Session finished")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Stopped")
