#!/usr/bin/env python3
"""Step 1 / step 2 of OnAir Web against a fake directory Worker (tools/fake_directory.py): the listing program (onair-server --directory-run), the preview, the
status file, the "Test connection" lines and every way the directory or the network can misbehave. usage: test_fake_directory.py ONAIR_SERVER FAKE_DIRECTORY.py"""
import http.server, json, os, shutil, signal, socket, subprocess, sys, tempfile, threading, time, urllib.request

SERVER, FAKE = sys.argv[1], sys.argv[2]
fails = []
def check(cond, what):
    if not cond:
        fails.append(what); print("FAIL:", what, flush=True)
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def post(url, obj):
    req = urllib.request.Request(url, json.dumps(obj).encode(), {"Content-Type": "application/json"})
    return urllib.request.urlopen(req, timeout=5).read()
def get(url):
    return json.loads(urllib.request.urlopen(url, timeout=5).read())
def wait_for(pred, seconds, step=0.25):
    end = time.time() + seconds
    while time.time() < end:
        v = pred()
        if v: return v
        time.sleep(step)
    return None

tmp = tempfile.mkdtemp(prefix="webshare-test-")
procs = []
def spawn(args, **kw):
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw); procs.append(p); return p

# a stand-in for the local OnAir web server: /web/v1/info
class Info(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = b'{"proto":1,"name":"Roof","users":2,"max_users":8,"capture":{"centre_hz":97750000,"rate_hz":20000000}}'
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
web_port = free_port()
info = http.server.ThreadingHTTPServer(("127.0.0.1", web_port), Info)
threading.Thread(target=info.serve_forever, daemon=True).start()

def conf_text(dir_url, ack="", extra="", public="https://roof-sdr.trycloudflare.com", port=None):
    return "\n".join([
        "web=1", "web_port=%d" % (port or web_port), "web_name=Roof", "web_max_users=8", "web_public_url=" + public, "web_directory=1", "web_directory_url=" + dir_url,
        "web_directory_key=" + os.path.join(tmp, "web_directory.key"), "web_directory_loc=54.6872,25.2797", "web_directory_loc_decimals=2", "web_directory_freq=88-108",
        "web_directory_radio=HackRF Pro", "web_directory_desc=FM and air", "web_directory_reach=0", "web_directory_ack=" + ack, extra, ""])
def write_conf(dir_url, ack="", **kw):
    p = os.path.join(tmp, "directory.conf"); open(p, "w").write(conf_text(dir_url, ack, **kw)); return p
def status():
    try: return json.load(open(os.path.join(tmp, "web_directory_status.json")))
    except Exception: return None
def clear_status():
    try: os.remove(os.path.join(tmp, "web_directory_status.json"))
    except OSError: pass
def preview(dir_url, **kw):
    out = subprocess.run([SERVER, "--config", write_conf(dir_url, **kw), "--directory-preview"], capture_output=True, text=True, timeout=30).stdout
    ack = out.split("\n", 1)[0]
    return (ack[4:] if ack.startswith("ack=") else None), out
def run_listing(dir_url, ack, **kw):
    clear_status()
    return spawn([SERVER, "--config", write_conf(dir_url, ack, **kw), "--directory-run"])
def stop(p, limit=6):
    t0 = time.time(); p.send_signal(signal.SIGTERM)
    try: p.wait(timeout=limit)
    except subprocess.TimeoutExpired: p.kill(); check(False, "the listing program did not stop within %d s" % limit)
    return time.time() - t0

dport = free_port()
fake = spawn([sys.executable, FAKE, str(dport)])
check(fake.stdout.readline().strip() == "ready", "fake directory starts")
D = "http://127.0.0.1:%d" % dport
def mode(m, **kw): post(D + "/__mode", dict(mode=m, **kw))

try:
    # ---- the preview needs no web server, shows the exact body and a code
    ack, out = preview(D)
    check(ack and len(ack) == 12 and "api/v1/heartbeat" in out and '"name":"Roof"' in out and '"freq":[[88000000,108000000]]' in out, "preview shows the exact body and a 12 character code: " + out[:200])
    check("<signature made with this server's key>" in out and "web_directory_reach" not in out, "preview does not contain a real signature")
    ack2, _ = preview(D)
    check(ack == ack2, "the code is stable")
    ackc, _ = preview(D, extra="web_directory_desc=changed")
    check(ackc != ack, "changing a field changes the code")
    # a bad location is reported as a sentence, not a crash
    r = subprocess.run([SERVER, "--config", write_conf(D, extra="web_directory_loc=north,east"), "--directory-preview"], capture_output=True, text=True, timeout=30)
    check(r.returncode != 0 and r.stdout.startswith("error=") and "latitude" in r.stdout, "bad location gives an error line: " + r.stdout)
    # nothing is sent without the code
    p = spawn([SERVER, "--config", write_conf(D, "wrongcode0000"), "--directory-run"]); p.wait(timeout=20)
    check(p.returncode != 0 and "nothing is sent yet" in (p.stdout.read() or ""), "a wrong code sends nothing")
    check(get(D + "/__log") == [], "the directory received nothing yet")

    # ---- A: the normal case
    mode("ok")
    ack, _ = preview(D)
    p = run_listing(D, ack)
    st = wait_for(lambda: (s := status()) and s["state"] == "ok" and s, 25)
    check(st and st["http"] == 200 and st["last_ok"] > 0 and st["id"].startswith("onair-"), "A: listed (state ok)")
    log = get(D + "/__log")
    check(len(log) >= 1 and log[0]["name"] == "Roof" and log[0]["users"] == 2 and log[0]["cap"] == 8 and log[0]["freq"] == [[88000000, 108000000]] and log[0]["radio"] == "HackRF Pro", "A: the directory got a valid signed heartbeat with the live user count: %s" % log[:1])
    check(stop(p) < 5, "A: stops quickly")

    # ---- B: the directory is down (500 with an HTML page): a clear message, then it recovers by itself
    mode("http500")
    p = run_listing(D, ack)
    st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 25)
    check(st and st["code"] == "server" and st["http"] == 500 and "Nothing is wrong with your setup" in st["advice"] and st["next_try"] > st["last_try"], "B: 500 explained")
    mode("ok")
    st = wait_for(lambda: (s := status()) and s["state"] == "ok" and s, 45)
    check(st and st["fails"] == 0, "B: recovered by itself after the directory came back")
    stop(p)

    # ---- C: a fresh address that DNS does not know yet (422) is retried in seconds, not minutes
    mode("resolving", n=2)
    p = run_listing(D, ack)
    st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 15)
    check(st and st["code"] == "resolving" and st["retry_soon"] and "keeps trying" in st["advice"], "C: 422 explained as temporary")
    t0 = time.time()
    st = wait_for(lambda: (s := status()) and s["state"] == "ok" and s, 40)
    check(st is not None and time.time() - t0 < 30, "C: listed after two retries within seconds (took %.0f s)" % (time.time() - t0))
    stop(p)

    # ---- D: the directory accepts the connection and never answers: the request ends at its time limit and says so, and stopping does not wait for it
    mode("hang")
    p = run_listing(D, ack)
    st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 25)
    check(st and st["code"] == "timeout" and "slow or blocks" in st["advice"], "D: hang becomes a timeout message (%s)" % (st and st["code"]))
    mode("ok")
    st = wait_for(lambda: (s := status()) and s["state"] == "ok" and s, 40)
    check(st is not None, "D: and it lists once the directory answers again")
    mode("hang")
    time.sleep(25)   # the next beat is now waiting for an answer
    check(stop(p, 6) < 6, "D: stop cancels a request that is in flight")
    mode("ok")

    # ---- E: nothing listens at the directory address: refused
    dead = "http://127.0.0.1:%d" % free_port()
    ackd, _ = preview(dead)
    p = run_listing(dead, ackd)
    st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 20)
    check(st and st["code"] == "refused" and "firewall" in st["advice"], "E: refused explained (%s)" % (st and st["code"]))
    stop(p)

    # ---- F: wrong clock, seen from the directory's answer
    mode("clock")
    p = run_listing(D, ack)
    st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 20)
    check(st and st["code"] == "clock" and "automatic date and time" in st["advice"] and st["have_skew"] and abs(st["skew_sec"] - 3600) < 120, "F: clock skew explained (%s)" % (st and st["code"]))
    stop(p)
    mode("ok")

    # ---- G: other answers
    for m, code in (("http429", "rate"), ("http401", "key"), ("html502", "server"), ("badjson", "other")):
        mode(m)
        p = run_listing(D, ack)
        st = wait_for(lambda: (s := status()) and s["state"] in ("error", "ok") and s, 20)
        if m == "badjson": check(st and st["state"] == "ok", "G: a 200 answer is a success whatever the body")
        else: check(st and st["code"] == code and st["advice"], "G: %s -> %s (%s)" % (m, code, st and st["code"]))
        stop(p)
    mode("ok")

    # ---- H: the web server on this computer is down: nothing is listed, and the message says what to do
    deadweb = free_port()
    clear_status()
    p = spawn([SERVER, "--config", write_conf(D, ack, port=deadweb), "--directory-run"])
    st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 20)
    check(st and st["code"] == "local" and "Start sharing" in st["advice"], "H: local server down explained (%s)" % (st and st["code"]))
    stop(p)

    # ---- I: Test connection
    r = subprocess.run([SERVER, "--config", write_conf(D, ack, public="https://127.0.0.1.invalid-test-host.example.org"), "--directory-test"], capture_output=True, text=True, timeout=60)
    steps = [json.loads(l) for l in r.stdout.splitlines() if l.startswith("{")]
    by = {s["step"]: s for s in steps}
    check(by.get("directory", {}).get("ok") is True and by["directory"]["ms"] >= 0, "I: directory step ok")
    check(by.get("local", {}).get("ok") is True, "I: local step ok")
    check("public" in by and by["public"]["ok"] is False and by["public"]["advice"] and "tunnel" in by["public"]["advice"], "I: public step fails with advice")
    check(steps and steps[-1]["step"] == "done", "I: ends with done")
    mode("hang")   # a health route that answers is unaffected by heartbeat modes, but a dead directory is:
    r = subprocess.run([SERVER, "--config", write_conf(dead, ack), "--directory-test"], capture_output=True, text=True, timeout=60)
    steps = {json.loads(l)["step"]: json.loads(l) for l in r.stdout.splitlines() if l.startswith("{")}
    check(steps["directory"]["ok"] is False and steps["directory"]["code"] == "refused" and steps["directory"]["advice"], "I: dead directory explained")
    mode("ok")

    # ---- J: a certificate that is not trusted (self-signed) is named as such
    if shutil.which("openssl"):
        crt, key = os.path.join(tmp, "c.pem"), os.path.join(tmp, "k.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"], capture_output=True, check=True)
        tport = free_port()
        tfake = spawn([sys.executable, FAKE, str(tport), "--tls", crt, key])
        check(tfake.stdout.readline().strip() == "ready", "J: tls fake starts")
        T = "https://localhost:%d" % tport
        ackt, out = preview(T)
        check(ackt is not None, "J: preview works for an https directory")
        p = run_listing(T, ackt)
        st = wait_for(lambda: (s := status()) and s["state"] == "error" and s, 25)
        check(st and st["code"] == "tls_cert" and "date and time" in st["advice"], "J: untrusted certificate explained (%s)" % (st and st["code"]))
        stop(p)
        # and trusted when the owner points the system at it (curl honours CURL_CA_BUNDLE): the request goes all the way through
        env = dict(os.environ, CURL_CA_BUNDLE=crt, SSL_CERT_FILE=crt)
        clear_status()
        p = spawn([SERVER, "--config", write_conf(T, ackt), "--directory-run"], env=env)
        st = wait_for(lambda: (s := status()) and s["state"] in ("ok", "error") and s, 25)
        if st and st["state"] != "ok": print("note: this curl ignores CURL_CA_BUNDLE (%s); the https success path is not checked here" % st["code"])
        stop(p)
finally:
    for p in procs:
        if p.poll() is None: p.kill()
    shutil.rmtree(tmp, ignore_errors=True)
print("webshare_fake_directory:", "all passed" if not fails else "%d FAILED" % len(fails))
sys.exit(1 if fails else 0)
