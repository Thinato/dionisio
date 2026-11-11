#!/usr/bin/env python3
"""End-to-end API test. Stdlib only. Usage: api_test.py <path-to-dionisio-server>"""
import datetime as dt
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

SERVER = sys.argv[1]
FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
JUNTOS = open(os.path.join(FIX, "pedido-71205254322071e20101.json"), "rb").read()
BEES = open(os.path.join(FIX, "bees-pedido-9354967539.json"), "rb").read()
ANCHOR = dt.date(2026, 11, 9)
TODAY = dt.date(2026, 11, 11)
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg, file=sys.stderr)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def next_boundary_after(d):
    return ANCHOR + dt.timedelta(days=14 * ((d - ANCHOR).days // 14 + 1))


class Server:
    def __init__(self, **extra):
        self.tmp = tempfile.mkdtemp()
        self.port = free_port()
        env = dict(os.environ, PORT=str(self.port), DB_PATH=os.path.join(self.tmp, "t.db"), API_TOKEN="t",
                   ALLOW_TODAY_OVERRIDE="1", CYCLE_ANCHOR=ANCHOR.isoformat(), GCS_BUCKET="", SLACK_BOT_TOKEN="")
        env.update(extra)
        self.proc = subprocess.Popen([SERVER], env=env)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.1).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def stop(self):
        self.proc.terminate()
        self.proc.wait(5)

    def call(self, method, path, body=None, token="t", headers=None, today=None):
        if today is not None:
            path += ("&" if "?" in path else "?") + "today=" + today.isoformat()
        data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data, method=method)
        if token is not None:
            req.add_header("Authorization", "Bearer " + token)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                status, raw = r.status, r.read()
        except urllib.error.HTTPError as e:
            status, raw = e.code, e.read()
        try:
            return status, json.loads(raw)
        except ValueError:
            return status, raw.decode()


def test_auth(s):
    routes = [("GET", "/api/home"), ("POST", "/api/orders"), ("GET", "/api/orders/1"), ("POST", "/api/orders/1"),
              ("GET", "/api/items"), ("POST", "/api/items/1"), ("GET", "/api/requests"), ("GET", "/api/cycles"),
              ("POST", "/jobs/daily"), ("GET", "/api/nope")]
    for m, p in routes:
        body = b"{}" if m == "POST" else None
        check(s.call(m, p, body, token=None)[0] == 401, f"AC7 no token {m} {p}")
        check(s.call(m, p, body, token="wrong")[0] == 401, f"AC7 wrong token {m} {p}")
        check(s.call(m, p, body, token="tt")[0] == 401, f"AC7 longer token {m} {p}")
        check(s.call(m, p + "?token=t", body, token=None)[0] == 401, f"AC7 query token {m} {p}")
    check(s.call("GET", "/api/home", token=None, headers={"X-Api-Token": "t"})[0] == 200, "X-Api-Token accepted")


def test_import(s):
    st, b = s.call("POST", "/api/orders", JUNTOS, today=TODAY)
    check(st == 201 and b["supplier"] == "JUNTOS" and b["lines"] == 21, f"AC1 import JUNTOS {st} {b}")
    st, b = s.call("POST", "/api/orders", BEES, today=TODAY)
    check(st == 201 and b["supplier"] == "BEES" and b["lines"] == 7, f"AC1 import BEES {st} {b}")
    check(s.call("POST", "/api/orders", JUNTOS, today=TODAY)[0] == 409, "AC1 re-import JUNTOS 409")
    check(s.call("POST", "/api/orders", BEES, today=TODAY)[0] == 409, "AC1 re-import BEES 409")
    check(s.call("POST", "/api/orders", b'{"orderId":"x","items":[{"name":"a"}]}')[0] == 400, "unknown format 400")
    check(s.call("POST", "/api/orders", b"nope")[0] == 400, "bad json 400")

    st, items = s.call("GET", "/api/items")
    check(st == 200 and len(items) == 28, f"AC1 28 items, got {len(items)}")
    check(sum(i["supplier"] == "JUNTOS" for i in items) == 21, "AC1 21 JUNTOS items")
    packs = {i["sku"]: i["pack_size"] for i in items if i["supplier"] == "BEES"}
    check(packs == {"0019229": 6, "0019227": 4, "0036434": 4, "0009085": 12, "0009274": 12, "0009093": 12,
                    "0006905": 12}, f"AC1 pack sizes {packs}")
    check(all(i["pack_size"] is None and i["active"] for i in items if i["supplier"] == "JUNTOS"), "JUNTOS no pack")

    st, home = s.call("GET", "/api/home", today=TODAY)
    check(home["cycle"] == {"start": "2026-11-09", "next_boundary": "2026-11-23"}, f"cycle {home['cycle']}")
    by = {o["supplier"]: o for o in home["orders"]}
    check(by["JUNTOS"]["boleto"]["amount_due_cents"] == 100772, "AC1 JUNTOS amount 100772")
    check(by["BEES"]["boleto"]["amount_due_cents"] == 60241, "AC1 BEES amount 60241")
    check(all(o["boleto"]["due_date"] is None and o["boleto"]["paid_on"] is None for o in home["orders"]),
          "AC1 no due date, unpaid")
    check(by["JUNTOS"]["lines_total"] == 21 and by["JUNTOS"]["lines_done"] == 0, "lines_done/total")
    check(len(home["suggestions"]) == 28, "suggestions list every active item")
    return by["JUNTOS"]["id"], by["BEES"]["id"]


def test_dedup(s):
    # sku-first: same sku, renamed -> no new item. Same name, new sku -> " [sku]" suffix, never 500.
    base = json.loads(BEES)
    renamed = dict(base, orderId="dedup-1", items=[dict(base["items"][0], name="Red Bull renamed")])
    check(s.call("POST", "/api/orders", json.dumps(renamed).encode(), today=TODAY)[0] == 201, "sku dedup import")
    collide = dict(base, orderId="dedup-2", items=[dict(base["items"][0], sku="9999999")])
    check(s.call("POST", "/api/orders", json.dumps(collide).encode(), today=TODAY)[0] == 201, "collision import")
    items = s.call("GET", "/api/items")[1]
    check(len(items) == 29, f"one new item from collision, got {len(items)}")
    check(any(i["name"].endswith(" [9999999]") and i["pack_size"] == 6 for i in items), "collision suffix")
    for oid in ("dedup-1", "dedup-2"):  # pay them so they stay out of home
        o = next(o for o in s.call("GET", "/api/home", today=TODAY)[1]["orders"] if o["external_id"] == oid)
        s.call("POST", f"/api/orders/{o['id']}", {"paid_on": "2026-11-11"})


def test_checklist(s, jid, bid):
    st, o = s.call("GET", f"/api/orders/{jid}")
    check(st == 200 and len(o["lines"]) == 21, "order detail lines")
    lines = {str(l["id"]): ("missing" if l["name"] == "Eisenbahn Unfiltered" else "arrived") for l in o["lines"]}
    st, o = s.call("POST", f"/api/orders/{jid}", {"lines": lines})
    check(st == 200 and o["boleto"]["amount_due_cents"] == 89516, f"AC2 JUNTOS 89516 {st} {o.get('boleto')}")
    check(o["lines_done"] == 21, "AC2 lines_done 21")
    sprite = next(l for l in o["lines"] if l["name"] == "Sprite Zero Açúcar")
    st, o = s.call("POST", f"/api/orders/{jid}", {"lines": {str(sprite["id"]): "pending"}})
    check(o["boleto"]["amount_due_cents"] == 89516 - 5419, "AC2 back to pending drops amount")
    st, o = s.call("POST", f"/api/orders/{jid}", {"lines": {str(sprite["id"]): "arrived"}})
    check(o["boleto"]["amount_due_cents"] == 89516, "AC2 re-arrive")

    bo = s.call("GET", f"/api/orders/{bid}")[1]
    st, bo = s.call("POST", f"/api/orders/{bid}", {"lines": {str(l["id"]): "arrived" for l in bo["lines"]}})
    check(bo["boleto"]["amount_due_cents"] == 60241, "AC2 BEES all arrived 60241")

    # validation
    for bad in ({"due_date": "2026-02-30"}, {"paid_on": "11/11/2026"}, {"linha_digitavel": "abc"},
                {"linha_digitavel": "1" * 65}, {"lines": {str(sprite["id"]): "eaten"}}, {"lines": []},
                {"lines": {str(bo["lines"][0]["id"]): "arrived"}}, [1]):
        check(s.call("POST", f"/api/orders/{jid}", bad)[0] == 400, f"400 for {bad}")
    check(s.call("GET", "/api/orders/999")[0] == 404, "unknown order 404")
    check(s.call("POST", "/api/orders/999", {})[0] == 404, "unknown order POST 404")

    st, o = s.call("POST", f"/api/orders/{jid}", {"due_date": "2026-11-20", "linha_digitavel": "1234.5678 9"})
    check(st == 200 and o["boleto"]["due_date"] == "2026-11-20" and o["boleto"]["linha_digitavel"] == "1234.5678 9",
          "boleto fields set")
    check(not o["boleto"]["overdue"], "not overdue")
    od = s.call("GET", f"/api/orders/{jid}", today=dt.date(2026, 11, 21))[1]
    check(od["boleto"]["overdue"], "overdue after due date")
    st, o = s.call("POST", f"/api/orders/{jid}", {"linha_digitavel": ""})
    check(o["boleto"]["linha_digitavel"] is None and o["boleto"]["due_date"] == "2026-11-20", "clear linha only")


def test_items(s):
    items = s.call("GET", "/api/items")[1]
    monster = next(i for i in items if i["name"] == "Monster Energy")
    st, it = s.call("POST", f"/api/items/{monster['id']}", {"pack_size": 6, "admin_min_packs": 3})
    check(st == 200 and it["pack_size"] == 6 and it["admin_min_packs"] == 3, f"item update {it}")
    sug = next(x for x in s.call("GET", "/api/home", today=TODAY)[1]["suggestions"] if x["item_id"] == monster["id"])
    check(sug["suggested"] == 3 and sug["packs"] == 0 and not sug["unknown_pack"], f"admin min floor {sug}")
    for bad in ({"pack_size": 0}, {"pack_size": "6"}, {"admin_min_packs": -1}, {"active": 1}):
        check(s.call("POST", f"/api/items/{monster['id']}", bad)[0] == 400, f"item 400 for {bad}")
    check(s.call("POST", "/api/items/999", {"active": False})[0] == 404, "unknown item 404")
    s.call("POST", f"/api/items/{monster['id']}", {"admin_min_packs": 0})
    return monster["id"]


def test_cycles(s, jid, bid):
    later = TODAY + dt.timedelta(days=14)
    home = s.call("GET", "/api/home", today=later)[1]
    check(home["cycle"]["start"] == "2026-11-23", "next cycle")
    ids = {o["id"]: o for o in home["orders"]}
    check(set(ids) == {jid, bid} and ids[jid]["cycle_start"] == "2026-11-09", "home keeps unpaid past orders")
    s.call("POST", f"/api/orders/{bid}", {"paid_on": "2026-11-24"})
    home = s.call("GET", "/api/home", today=later)[1]
    check({o["id"] for o in home["orders"]} == {jid}, "paid past order leaves home")
    cycles = s.call("GET", "/api/cycles", today=later)[1]
    past = next(c for c in cycles if c["start"] == "2026-11-09")
    check(any(o["id"] == bid and o["boleto"]["paid_on"] == "2026-11-24" for o in past["orders"]), "paid in cycles")
    # current-cycle paid order stays on home
    check(any(o["id"] == bid for o in s.call("GET", "/api/home", today=TODAY)[1]["orders"]), "current paid on home")


def main():
    s = Server()
    try:
        test_auth(s)
        jid, bid = test_import(s)
        test_dedup(s)
        test_checklist(s, jid, bid)
        test_items(s)
        test_cycles(s, jid, bid)
    finally:
        s.stop()
    if failures:
        print(f"{len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    print("api_test: all checks passed")


if __name__ == "__main__":
    main()
