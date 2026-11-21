#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
"""Soak test of the headless server on its built-in test signal.

  soak_server.py --pid PID --port PORT --minutes 60 [--interval 30] [--max-rss-growth 25]

Watches a running onair-server (started by the caller on device=test with autostart, loopback only; see .github/workflows/soak.yml): every
--interval seconds it reads /api/v1/metrics and the process's resident memory and thread count (Linux /proc). Fails when the server dies, the receiver stops, samples keep being dropped, or the resident memory
at the end is more than --max-rss-growth percent above the figure after the first quarter of the run (warm-up). Prints a table.
"""
import argparse
import http.client
import os
import re
import sys
import time


def proc_stats(pid):
    rss = threads = 0
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1])   # kB
            elif line.startswith("Threads:"):
                threads = int(line.split()[1])
    return rss, threads


def metric(text, name):
    m = re.search(r"^" + re.escape(name) + r" ([0-9.eE+-]+)$", text, re.M)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--interval", type=float, default=30)
    ap.add_argument("--max-rss-growth", type=float, default=25)
    ap.add_argument("--port", type=int, required=True)
    a = ap.parse_args()
    rows, fail = [], []
    end = time.time() + a.minutes * 60
    t0 = time.time()
    while time.time() < end:
        time.sleep(a.interval)
        if not os.path.exists(f"/proc/{a.pid}"):
            fail.append("the server exited")
            break
        try:
            conn = http.client.HTTPConnection("127.0.0.1", a.port, timeout=10)
            conn.request("GET", "/api/v1/metrics")
            txt = conn.getresponse().read().decode()
            conn.close()
        except Exception as e:   # noqa: BLE001
            fail.append(f"/metrics did not answer: {e}")
            break
        rss, th = proc_stats(a.pid)
        rows.append((int(time.time() - t0), rss, th, metric(txt, "onair_receiver_running"), metric(txt, "onair_dropped_samples_total")))
    print("| seconds | RSS MB | threads | running | dropped samples |\n|--:|--:|--:|--:|--:|")
    for s, rss, th, run, dr in rows:
        print(f"| {s} | {rss / 1024:.1f} | {th} | {run} | {dr} |")
    if len(rows) >= 4:
        warm = rows[len(rows) // 4][1]
        last = rows[-1][1]
        growth = (last / warm - 1) * 100 if warm else 0
        print(f"\nRSS after warm-up {warm / 1024:.1f} MB, at the end {last / 1024:.1f} MB ({growth:+.1f} %); threads {rows[len(rows) // 4][2]} -> {rows[-1][2]}")
        if growth > a.max_rss_growth:
            fail.append(f"resident memory grew {growth:.1f} % after warm-up")
        if rows[-1][2] > rows[len(rows) // 4][2] + 8:
            fail.append("the thread count keeps growing")
        if any(r[3] != 1 for r in rows):
            fail.append("the receiver was not running at every sample")
    else:
        fail.append("too few samples")
    for f in fail:
        print("FAIL:", f)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
