#!/usr/bin/env python3
"""GCS persistence tests against a fake GCS. Usage: gcs_test.py <dionisio-server>. Stdlib only."""
import json, os, socket, subprocess, sys, tempfile, threading, time, urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SERVER = sys.argv[1]
FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
TOKEN = "t"


class Gcs(BaseHTTPRequestHandler):
    obj, gen, get_status, fail_uploads = None, 0, None, 0  # class-level state: the one bucket object

    def log_message(self, *a):
        pass

    def reply(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if Gcs.get_status:
            return self.reply(Gcs.get_status)
        if Gcs.obj is None:
            return self.reply(404)
        self.reply(200, Gcs.obj, [("x-goog-generation", str(Gcs.gen))])

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/_hook/replace"):  # swap in another snapshot with a higher generation
            Gcs.obj, Gcs.gen = body, Gcs.gen + 10
            return self.reply(200)
        if Gcs.fail_uploads:
            Gcs.fail_uploads -= 1
            return self.reply(503)
        want = self.path.split("ifGenerationMatch=")[1].split("&")[0]
        if want != str(Gcs.gen):
            return self.reply(412)
        Gcs.obj, Gcs.gen = body, Gcs.gen + 1
        self.reply(200, json.dumps({"generation": str(Gcs.gen)}).encode())


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


fake = ThreadingHTTPServer(("127.0.0.1", 0), Gcs)
threading.Thread(target=fake.serve_forever, daemon=True).start()
tmp = tempfile.mkdtemp()


def start(name):
    port = free_port()
    env = dict(os.environ, PORT=str(port), DB_PATH=os.path.join(tmp, name + ".db"), API_TOKEN=TOKEN,
               GCS_BUCKET="b", GCS_ENDPOINT="http://127.0.0.1:%d" % fake.server_address[1],
               ALLOW_TODAY_OVERRIDE="1", SLACK_SIGNING_SECRET="x", ADMIN_SLACK_IDS="UA")
    p = subprocess.Popen([SERVER], env=env)
    return p, "http://127.0.0.1:%d" % port


def wait_up(p, base):
    for _ in range(100):
        assert p.poll() is None, "server exited early"
        try:
            urllib.request.urlopen(base + "/api/home")
        except urllib.error.HTTPError:
            return  # 401 = up
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server never came up")


def call(base, method, path, body=None, raw=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(base + path, data=data, method=method, headers={"Authorization": "Bearer " + TOKEN})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, None


def fixture(name):
    return open(os.path.join(FIX, name), "rb").read()


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("ok:", msg)


JUNTOS, BEES = fixture("pedido-71205254322071e20101.json"), fixture("bees-pedido-9354967539.json")

# --- restore on start + generation bumps
p, base = start("one")
try:
    wait_up(p, base)
    check(Gcs.obj is None, "empty bucket: fresh start (404)")
    check(call(base, "POST", "/api/orders", raw=JUNTOS)[0] == 201, "import JUNTOS")
    check(Gcs.gen == 1, "first change uploads generation 1")
    snap1 = Gcs.obj
    check(call(base, "POST", "/api/orders", raw=BEES)[0] == 201, "import BEES")
    check(Gcs.gen == 2, "second change bumps to 2")
    call(base, "GET", "/api/home")
    call(base, "GET", "/api/items")
    check(call(base, "POST", "/api/orders", raw=BEES)[0] == 409, "re-import is 409")
    check(Gcs.gen == 2, "GETs and no-op mut requests do not bump the generation")

    # --- a failed upload is not forgotten: the client's retry (a no-op here) uploads it
    Gcs.fail_uploads = 1
    nodue = dict(json.loads(JUNTOS), orderId="retry-1")
    check(call(base, "POST", "/api/orders", body=nodue)[0] == 503, "upload 503 -> request 503")
    check(Gcs.gen == 2, "nothing uploaded yet")
    check(call(base, "POST", "/api/orders", body=nodue)[0] == 409, "retry is a 409 (row is local)")
    check(Gcs.gen == 3, "but the retry uploads the pending change")
    check(call(base, "POST", "/api/orders", body=nodue)[0] == 409 and Gcs.gen == 3, "then nothing more to upload")
finally:
    p.terminate(); p.wait()

p, base = start("two")  # fresh local DB: must restore from the fake bucket
try:
    wait_up(p, base)
    s, home = call(base, "GET", "/api/home")
    check(s == 200 and len(home["orders"]) == 3, "second server restores every order from GCS")

    # --- forced 412: another writer replaced the object with an older snapshot
    item_id = min(i["id"] for i in call(base, "GET", "/api/items")[1])  # a JUNTOS item, present in snap1
    conn = urllib.request.Request("http://127.0.0.1:%d/_hook/replace" % fake.server_address[1], data=snap1, method="POST")
    urllib.request.urlopen(conn)
    check(call(base, "POST", "/api/items/%d" % item_id, {"admin_min_packs": 1})[0] == 503, "stale generation: 503")
    s, home = call(base, "GET", "/api/home")
    check(p.poll() is None and len(home["orders"]) == 1, "process still up, reloaded the swapped-in state")
    before = Gcs.gen
    st = call(base, "POST", "/api/items/%d" % item_id, {"admin_min_packs": 1})[0]
    check(st == 200, "next change succeeds (got %s)" % st)
    check(Gcs.gen == before + 1, "against the new generation")
finally:
    p.terminate(); p.wait()

# --- startup GET failure: must not start on an empty DB
Gcs.get_status = 500
p, _ = start("three")
try:
    check(p.wait(timeout=10) != 0, "startup GET 500 exits non-zero")
finally:
    if p.poll() is None:
        p.kill()
print("all gcs tests passed")
