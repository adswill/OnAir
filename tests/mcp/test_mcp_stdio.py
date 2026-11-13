#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""A real MCP client against the real program: `onair-server --mcp-stdio` as a child process on the built-in test signal.
stdout must carry JSON-RPC lines and nothing else (the log goes to stderr); then the same over HTTP (`--mcp-http`)."""
import json, os, subprocess, sys, tempfile, time, urllib.request

exe = sys.argv[1]
cfg = os.path.join(tempfile.mkdtemp(prefix="onair_mcp_"), "s.conf")
fails = 0

def check(c, msg):
    global fails
    if not c:
        print("FAIL:", msg); fails += 1

p = subprocess.Popen([exe, "--config", cfg, "--device", "test", "--mcp-stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
nid = [0]

def rpc(method, params=None, notify=False):
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None: msg["params"] = params
    if not notify:
        nid[0] += 1; msg["id"] = nid[0]
    p.stdin.write(json.dumps(msg) + "\n"); p.stdin.flush()
    if notify: return None
    line = p.stdout.readline()
    check(line != "", "an answer for " + method)
    out = json.loads(line)          # raises if the stdout line is not JSON: that would break a client
    check(out.get("id") == msg["id"], "id echoed for " + method)
    return out

r = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "pytest", "version": "1"}})
check(r["result"]["serverInfo"]["name"] == "onair", "serverInfo")
rpc("notifications/initialized", notify=True)
tools = rpc("tools/list")["result"]["tools"]
check(len(tools) >= 20 and not any(t["name"].startswith("tx_") for t in tools), "tools listed, no TX tools")
def call(name, args):
    res = rpc("tools/call", {"name": name, "arguments": args})["result"]
    return res["isError"], json.loads(res["content"][0]["text"]) if not res["isError"] else res["content"][0]["text"]
e, _ = call("set_mode", {"mode": "drm", "frequency_mhz": 15.2}); check(not e, "set_mode drm")
e, _ = call("stop_receiver", {}); check(not e, "stop")
e, d = call("auto_decode", {"frequency_mhz": 15, "span_khz": 1000, "mode": "drm", "timeout_s": 25})
check(not e and d["locked"] and d["mode"]["id"] == "drm" and abs(d["frequency_mhz"] - 15.0) < 0.6, "find DRM near 15 MHz and decode it: " + str(d)[:300])
p.stdin.close()
try: p.wait(timeout=20)
except Exception: p.kill(); check(False, "the program exits at end of input")
err = p.stderr.read()
check("mcp auto_decode" in err, "the activity is logged on stderr")

# streamable HTTP
q = subprocess.Popen([exe, "--config", cfg, "--device", "test", "--port", "18765", "--mcp-http", "18766", "--mcp-token", "0123456789abcdef0123"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
def post(body, token=True):
    rq = urllib.request.Request("http://127.0.0.1:18766/mcp", data=json.dumps(body).encode(), headers={"Content-Type": "application/json", **({"Authorization": "Bearer 0123456789abcdef0123"} if token else {})})
    try:
        with urllib.request.urlopen(rq, timeout=60) as f: return f.status, f.read().decode()
    except urllib.error.HTTPError as ex: return ex.code, ex.read().decode()
try:
    for _ in range(50):
        try: s, _b = post({"jsonrpc": "2.0", "id": 1, "method": "ping"}); break
        except Exception: time.sleep(0.2)
    s, b = post({"jsonrpc": "2.0", "id": 1, "method": "ping"}); check(s == 200, "HTTP ping %s" % s)
    s, b = post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, token=False); check(s == 401, "HTTP without token %s" % s)
    s, b = post({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "list_modes", "arguments": {"query": "ais"}}}); check(s == 200 and "AIS" in b, "HTTP tool call")
finally:
    q.terminate(); q.wait(timeout=20)
print("mcp_stdio:", "FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)
