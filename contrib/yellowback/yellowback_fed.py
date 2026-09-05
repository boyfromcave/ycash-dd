#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Yellowback federation coordinator (plan §5, D6, D16).

One process per operator, beside that operator's ycashd. It never holds a
key: every signature is made by the node through signrawtransaction (anchor
spends) and yed_cosignredeem (vault spends). It is the only component that
talks to the internet.

Roles (all in one process):
  * price rounds: fetch YEC/USD from >= 3 sources, TWAP per source, drop
    silent sources, 10 % outlier filter, median, +/-10 % move clamp against
    the last on-chain price; the proposer of the round builds the PRICE
    transaction with yed_createpricetx, signs, POSTs the unsigned hex to
    peers (/pricesign), merges the partials with one signrawtransaction
    call (F2) and broadcasts;
  * peer verification of a proposed PRICE transaction (§5 step 4): price
    within 2 % of the peer's own median, structure exact, no input other
    than the anchor solvable by this wallet (C5);
  * POST /cosign: redemption co-signing through yed_cosignredeem, with
    per-IP rate limiting;
  * rotate: ROTATION then PRICE from the new anchor (C9).

Test hooks (plan G7): --mock-price <file> reads the USD price from a file
the test rewrites; --insecure-localhost serves and talks plain HTTP on
127.0.0.1 (production is HTTPS with operator client certificates, which is
deployment configuration, not code).

Usage:
  yellowback_fed.py serve  --config fed.toml | --rpc-url ... --id N --listen HOST:PORT --peers URL,URL ...
  yellowback_fed.py rotate --new-roster <hex> [same connection options]
  yellowback_fed.py status
"""

import argparse
import base64
import json
import logging
import os
import statistics
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

LOG = logging.getLogger("yellowback-fed")

MICRO = 1_000_000
YELLOWBACK_FEE = 1000
PRICE_MIN = 100
PRICE_MAX = 100_000_000
DEFAULT_ROUND_BLOCKS = 8
MOVE_TRIGGER_BPS = 100        # >= 1 % move starts a round early
CLAMP_BPS = 1000              # +/- 10 % per round
PEER_TOLERANCE_BPS = 200      # peers accept within 2 % of their own median
OUTLIER_BPS = 1000            # sources more than 10 % from the median are dropped
TWAP_SECONDS = 300
SOURCE_SILENCE_SECONDS = 120
MIN_SOURCES = 3
REFILL_BELOW_ZAT = 1_000_000  # 0.01 YEC
REFILL_AMOUNT_YEC = "0.5"
ROUND_TIMEOUT_BLOCKS = 4


# ---------------------------------------------------------------------------
# JSON-RPC client (stdlib only)

class RpcError(Exception):
    def __init__(self, code, message):
        super().__init__("%s (code %s)" % (message, code))
        self.code = code
        self.message = message


class Node:
    def __init__(self, url, timeout=60):
        self.url = url
        self.timeout = timeout
        auth = None
        if "@" in url:
            scheme, rest = url.split("://", 1)
            creds, host = rest.rsplit("@", 1)
            auth = base64.b64encode(creds.encode()).decode()
            self.url = "%s://%s" % (scheme, host)
        self.auth = auth
        self._id = 0

    def call(self, method, *params):
        self._id += 1
        body = json.dumps({"jsonrpc": "1.0", "id": self._id, "method": method, "params": list(params)}).encode()
        req = urllib.request.Request(self.url, data=body, headers={"Content-Type": "application/json"})
        if self.auth:
            req.add_header("Authorization", "Basic " + self.auth)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                reply = json.loads(resp.read().decode())
        except urllib.error.HTTPError as e:
            reply = json.loads(e.read().decode())
        if reply.get("error"):
            raise RpcError(reply["error"].get("code"), reply["error"].get("message"))
        return reply["result"]

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda *p: self.call(name, *p)


# ---------------------------------------------------------------------------
# Price sources

class PriceFeed:
    """TWAP per source, silence drop, outlier filter, median (plan §5 step 1, A16)."""

    def __init__(self, sources, mock_file=None):
        self.sources = sources          # list of {"name", "url", "path", "quote": "USD"|"BTC"}
        self.mock_file = mock_file
        self.samples = {}               # name -> [(t, micro_usd)]
        self.lock = threading.Lock()

    @staticmethod
    def _extract(obj, path):
        for part in path.split("."):
            if isinstance(obj, list):
                obj = obj[int(part)]
            else:
                obj = obj[part]
        return float(obj)

    def poll(self):
        now = time.time()
        if self.mock_file:
            try:
                with open(self.mock_file) as f:
                    usd = float(f.read().strip())
                with self.lock:
                    self.samples.setdefault("mock", []).append((now, int(round(usd * MICRO))))
            except (OSError, ValueError) as e:
                LOG.warning("mock price unreadable: %s", e)
            return
        btc_usd = []
        for s in self.sources:
            if s.get("quote", "USD") == "USD" and s.get("btc_usd"):
                try:
                    btc_usd.append(self._fetch(s["btc_usd"], s.get("btc_usd_path", "price")))
                except Exception as e:
                    LOG.warning("BTC/USD source failed: %s", e)
        btc_median = statistics.median(btc_usd) if btc_usd else None
        for s in self.sources:
            try:
                v = self._fetch(s["url"], s["path"])
                if s.get("quote", "USD") == "BTC":
                    if btc_median is None:
                        continue
                    v *= btc_median
                with self.lock:
                    self.samples.setdefault(s["name"], []).append((now, int(round(v * MICRO))))
            except Exception as e:
                LOG.warning("source %s failed: %s", s["name"], e)

    @staticmethod
    def _fetch(url, path):
        req = urllib.request.Request(url, headers={"User-Agent": "yellowback-fed/1"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            return PriceFeed._extract(json.loads(resp.read().decode()), path)

    def median_micro_usd(self):
        """Median of the per-source TWAPs, or None when fewer than MIN_SOURCES (1 in mock mode) are live."""
        now = time.time()
        twaps = []
        with self.lock:
            if self.mock_file:
                # Test mode: the file is the whole market; no averaging, so a test's price change is immediate.
                s = self.samples.get("mock")
                return s[-1][1] if s else None
            for name, samples in self.samples.items():
                samples[:] = [s for s in samples if now - s[0] <= TWAP_SECONDS]
                if not samples or now - samples[-1][0] > SOURCE_SILENCE_SECONDS:
                    continue
                twaps.append(sum(v for _, v in samples) / len(samples))
        needed = 1 if self.mock_file else MIN_SOURCES
        if len(twaps) < needed:
            return None
        med = statistics.median(twaps)
        kept = [t for t in twaps if abs(t - med) * 10000 <= OUTLIER_BPS * med]
        if len(kept) < needed:
            return None
        return int(round(statistics.median(kept)))


def clamp(price, last):
    if last is None or last <= 0:
        return price
    lo = last * (10000 - CLAMP_BPS) // 10000
    hi = last * (10000 + CLAMP_BPS) // 10000
    return max(lo, min(hi, price))


# ---------------------------------------------------------------------------
# Coordinator

class Coordinator:
    def __init__(self, cfg):
        self.cfg = cfg
        self.node = Node(cfg["rpc_url"])
        self.id = int(cfg["id"])
        self.peers = [p for p in cfg.get("peers", []) if p]
        self.feed = PriceFeed(cfg.get("sources", []), cfg.get("mock_price"))
        self.round_blocks = int(cfg.get("round_blocks", DEFAULT_ROUND_BLOCKS))
        self.insecure = bool(cfg.get("insecure_localhost", False))
        self.stop = threading.Event()
        self.last_round_height = -1
        self.last_round_txid = None
        self.rate = {}  # ip -> [timestamps]
        self.rate_lock = threading.Lock()
        self.status = {"rounds": 0, "cosigns": 0, "refusals": 0, "last_price": None, "last_round": None}

    # ---- node helpers
    def info(self):
        return self.node.yed_getinfo()

    def roster(self):
        return self.node.yed_getroster()

    def n(self):
        return int(self.roster()["n"])

    def k(self):
        return int(self.roster()["k"])

    def last_onchain_price(self):
        p = self.node.yed_getprice()
        return p["priceMicroUsd"], p["sourceHeight"]

    # ---- price round (proposer side)
    def my_turn(self, height, last_height):
        n = self.n()
        slot = (height // self.round_blocks) % n
        since = height - last_height if last_height >= 0 else self.round_blocks
        if slot == self.id:
            return True
        if since >= self.round_blocks + 2 and (slot + 1) % n == self.id:
            return True  # the proposer missed two blocks: next id takes over
        if since >= self.round_blocks + 4:
            return True  # everyone may try
        return False

    def round_due(self, height, last_height, price, last_price):
        if last_height < 0 or last_price is None:
            return True
        if height - last_height >= self.round_blocks:
            return True
        return abs(price - last_price) * 10000 >= MOVE_TRIGGER_BPS * last_price

    def ensure_refill(self, anchor_value):
        """When the anchor is nearly drained, prepare a confirmed UTXO of the exact refill amount (C6)."""
        if anchor_value >= REFILL_BELOW_ZAT:
            return None
        addr = self.node.getnewaddress()
        txid = self.node.sendtoaddress(addr, REFILL_AMOUNT_YEC)
        LOG.info("refill UTXO %s created; waiting for one confirmation", txid)
        while not self.stop.is_set():
            tx = self.node.gettransaction(txid)
            if tx.get("confirmations", 0) >= 1:
                for v in self.node.decoderawtransaction(tx["hex"])["vout"]:
                    if v["scriptPubKey"].get("addresses") == [addr]:
                        return "%s:%d" % (txid, v["n"])
            time.sleep(2)
        return None

    def run_round(self, price, rotate_script=None, roster_hint=None):
        info = self.info()
        refill = None if rotate_script else self.ensure_refill(int(info["anchor"]["valueZat"]))
        if rotate_script:
            built = self.node.yed_createpricetx("rotate", refill or "", rotate_script)
        else:
            built = self.node.yed_createpricetx(price, refill or "", roster_hint or "")
        unsigned = built["hex"]
        mine = self.node.signrawtransaction(unsigned)
        partials = [mine["hex"]]
        body = json.dumps({"hex": unsigned, "price": price, "rotate": rotate_script or ""}).encode()

        def ask(peer):
            try:
                r = self.http_post(peer + ("/rotatesign" if rotate_script else "/pricesign"), body)
                return r.get("hex")
            except Exception as e:
                LOG.warning("peer %s refused or failed: %s", peer, e)
                return None

        with ThreadPoolExecutor(max_workers=max(1, len(self.peers))) as ex:
            for h in ex.map(ask, self.peers):
                if h:
                    partials.append(h)
        merged = self.node.signrawtransaction("".join(partials))
        if not merged.get("complete"):
            LOG.warning("round at height %s did not reach k signatures (%d partials)", info["height"], len(partials))
            return None
        txid = self.node.sendrawtransaction(merged["hex"])
        self.status["rounds"] += 1
        self.status["last_round"] = {"height": info["height"], "txid": txid, "price": price, "rotate": bool(rotate_script)}
        if not rotate_script:
            self.status["last_price"] = price
        LOG.info("%s broadcast: %s price=%s", "rotation" if rotate_script else "price", txid, price)
        return txid

    def loop(self):
        LOG.info("coordinator %d serving; peers=%s", self.id, self.peers)
        while not self.stop.is_set():
            try:
                self.feed.poll()
                info = self.info()
                if not info["healthy"] or not info["synced"]:
                    time.sleep(1)
                    continue
                height = int(info["height"])
                if not info["anchor"]["valid"]:
                    time.sleep(5)
                    continue
                price = self.feed.median_micro_usd()
                last_price, last_height = self.last_onchain_price()
                if last_height is None or last_height < 0:
                    last_height = -1
                pending = self.last_round_txid and self.last_round_txid in self.node.getrawmempool()
                if price is not None and not pending and self.round_due(height, last_height, price, last_price) \
                        and self.my_turn(height, last_height) and height != self.last_round_height:
                    p = clamp(price, last_price)
                    p = max(PRICE_MIN, min(PRICE_MAX, p))
                    self.last_round_height = height
                    self.last_round_txid = self.run_round(p)
            except Exception as e:
                LOG.warning("loop error: %s", e)
            time.sleep(1)

    # ---- peer side
    def verify_price_tx(self, hex_, price, rotate_script):
        """§5 step 4: structure, price tolerance, and no foreign input this wallet can solve (C5)."""
        dec = self.node.decoderawtransaction(hex_)
        info = self.info()
        roster = self.roster()
        if len(dec["vin"]) < 1 or len(dec["vin"]) > 2:
            return "inputs: expected the anchor and at most one refill"
        anchor = info["anchor"]
        # vin[0] must be the index anchor, or a mempool spend chain of it (C11): accept if the spent
        # outpoint is the index anchor or an unconfirmed output paying the anchor script.
        v0 = dec["vin"][0]
        if not (v0["txid"] == anchor["txid"] and v0["vout"] == anchor["vout"]):
            prev = self.node.getrawtransaction(v0["txid"], 1) if v0["txid"] in self.node.getrawmempool() else None
            if not prev or prev["vout"][v0["vout"]]["scriptPubKey"].get("addresses") != [anchor["address"]]:
                return "vin[0] is not the anchor"
        # No non-anchor input may be one this wallet can solve.
        mine = set((u["txid"], u["vout"]) for u in self.node.listunspent(0))
        value_in = 0
        for i, vin in enumerate(dec["vin"]):
            if i > 0 and (vin["txid"], vin["vout"]) in mine:
                return "refill input belongs to this wallet (C5)"
            out = self.node.gettxout(vin["txid"], vin["vout"], True)
            if out is None:
                return "input %d unknown or spent" % i
            value_in += int(round(float(out["value"]) * 100_000_000))
        outs = dec["vout"]
        # A PRICE pays the anchor back to its own script (B8); a rotation pays the agreed new script.
        expected_addr = anchor["address"] if not rotate_script else self.node.decodescript(rotate_script)["p2sh"]
        if rotate_script:
            if len(outs) != 1:
                return "rotation must have exactly one output"
        else:
            if len(outs) != 2:
                return "price transaction must have exactly two outputs"
            payload = self.node.yed_decodepayload(hex_)
            if payload.get("type") != "price" or int(payload.get("priceMicroUsd", -1)) != int(price):
                return "OP_RETURN price does not match the stated price"
        if outs[0]["scriptPubKey"].get("addresses") != [expected_addr]:
            return "vout[0] is not the anchor script"
        value_out = sum(int(round(float(o["value"]) * 100_000_000)) for o in outs)
        if value_in - value_out != YELLOWBACK_FEE:
            return "fee is not YELLOWBACK_FEE"
        if not rotate_script:
            own = self.feed.median_micro_usd()
            if own is None:
                return "no own price to compare against"
            last_price, _ = self.last_onchain_price()
            own = clamp(own, last_price)
            if abs(int(price) - own) * 10000 > PEER_TOLERANCE_BPS * own:
                return "price %d differs from own median %d by more than 2%%" % (int(price), own)
        return None

    def sign_price_tx(self, hex_, price, rotate_script):
        err = self.verify_price_tx(hex_, price, rotate_script)
        if err:
            self.status["refusals"] += 1
            raise ValueError(err)
        return self.node.signrawtransaction(hex_)["hex"]

    def cosign(self, hex_):
        r = self.node.yed_cosignredeem(hex_)
        self.status["cosigns"] += 1
        return r

    def rate_ok(self, ip, limit=30, window=60):
        now = time.time()
        with self.rate_lock:
            hits = [t for t in self.rate.get(ip, []) if now - t < window]
            hits.append(now)
            self.rate[ip] = hits
            return len(hits) <= limit

    # ---- transport
    def http_post(self, url, body):
        if not self.insecure and not url.startswith("https://"):
            raise ValueError("peer URL must be https (or run with --insecure-localhost)")
        req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read().decode())
        except urllib.error.HTTPError as e:
            raise ValueError(json.loads(e.read().decode()).get("error", str(e)))


class Handler(BaseHTTPRequestHandler):
    coordinator = None

    def log_message(self, fmt, *args):
        LOG.debug("http " + fmt, *args)

    def _reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            c = self.coordinator
            st = dict(c.status)
            try:
                st["info"] = c.info()
                st["price"] = c.node.yed_getprice()
            except Exception as e:
                st["error"] = str(e)
            return self._reply(200, st)
        self._reply(404, {"error": "not found"})

    def do_POST(self):
        c = self.coordinator
        ip = self.client_address[0]
        if not c.rate_ok(ip):
            return self._reply(429, {"error": "rate limited", "transient": True})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            req = json.loads(self.rfile.read(length).decode() or "{}")
        except Exception:
            return self._reply(400, {"error": "bad json"})
        try:
            if self.path == "/cosign":
                r = c.cosign(req["hex"])
                LOG.info("cosign from %s: %d of %d", ip, r["quorumSignatures"], r["k"])
                return self._reply(200, {"hex": r["hex"], "quorumSignatures": r["quorumSignatures"], "k": r["k"], "complete": r["complete"]})
            if self.path == "/pricesign":
                return self._reply(200, {"hex": c.sign_price_tx(req["hex"], req["price"], None)})
            if self.path == "/rotatesign":
                return self._reply(200, {"hex": c.sign_price_tx(req["hex"], 0, req["rotate"])})
            return self._reply(404, {"error": "not found"})
        except RpcError as e:
            transient = "(transient)" in (e.message or "")
            LOG.info("refused %s from %s: %s", self.path, ip, e.message)
            c.status["refusals"] += 1
            return self._reply(409, {"error": e.message, "transient": transient})
        except ValueError as e:
            LOG.info("refused %s from %s: %s", self.path, ip, e)
            return self._reply(409, {"error": str(e), "transient": False})
        except Exception as e:
            LOG.exception("handler error")
            return self._reply(500, {"error": str(e), "transient": True})


# ---------------------------------------------------------------------------

def load_config(args):
    cfg = {}
    if args.config:
        if tomllib is None:
            raise SystemExit("tomllib not available; pass options on the command line")
        with open(args.config, "rb") as f:
            cfg = tomllib.load(f)
    for key in ("rpc_url", "id", "listen", "mock_price", "round_blocks"):
        v = getattr(args, key, None)
        if v is not None:
            cfg[key] = v
    if args.peers:
        cfg["peers"] = [p.strip() for p in args.peers.split(",") if p.strip()]
    if args.insecure_localhost:
        cfg["insecure_localhost"] = True
    for req in ("rpc_url", "id"):
        if req not in cfg:
            raise SystemExit("missing %s" % req)
    return cfg


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["serve", "rotate", "status"])
    ap.add_argument("--config")
    ap.add_argument("--rpc-url", dest="rpc_url", help="http://user:pass@127.0.0.1:port")
    ap.add_argument("--id", type=int)
    ap.add_argument("--listen", help="HOST:PORT for /cosign, /pricesign, /rotatesign, /status")
    ap.add_argument("--peers", help="comma-separated peer coordinator base URLs")
    ap.add_argument("--mock-price", dest="mock_price", help="TEST ONLY: read the USD price from this file")
    ap.add_argument("--insecure-localhost", dest="insecure_localhost", action="store_true", help="TEST ONLY: plain HTTP on 127.0.0.1")
    ap.add_argument("--round-blocks", dest="round_blocks", type=int)
    ap.add_argument("--new-roster", dest="new_roster", help="rotate: the new roster script hex")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s", stream=sys.stderr)
    cfg = load_config(args)
    c = Coordinator(cfg)

    if args.command == "status":
        print(json.dumps({"info": c.info(), "price": c.node.yed_getprice(), "roster": c.roster()}, indent=2))
        return 0
    if args.command == "rotate":
        if not args.new_roster:
            raise SystemExit("--new-roster required")
        txid = c.run_round(0, rotate_script=args.new_roster)
        if not txid:
            return 1
        # Wait for the rotation to confirm, then reveal the new roster with the first PRICE from the
        # new anchor (C9). The serving coordinators cannot build that PRICE: the script is unrevealed.
        LOG.info("rotation %s broadcast; waiting for it to confirm", txid)
        while not c.stop.is_set():
            info = c.info()
            if info["anchor"]["txid"] == txid and info["synced"]:
                break
            time.sleep(1)
        price = None
        for _ in range(60):
            c.feed.poll()
            price = c.feed.median_micro_usd()
            if price is not None:
                break
            time.sleep(1)
        if price is None:
            raise SystemExit("no price available for the reveal round")
        last_price, _ = c.last_onchain_price()
        reveal = c.run_round(max(PRICE_MIN, min(PRICE_MAX, clamp(price, last_price))), roster_hint=args.new_roster)
        print(json.dumps({"rotation": txid, "reveal": reveal}))
        return 0 if reveal else 1

    host, port = cfg["listen"].rsplit(":", 1)
    if host not in ("127.0.0.1", "localhost") and c.insecure:
        raise SystemExit("--insecure-localhost only serves on 127.0.0.1")
    Handler.coordinator = c
    server = ThreadingHTTPServer((host, int(port)), Handler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    try:
        c.loop()
    except KeyboardInterrupt:
        pass
    finally:
        c.stop.set()
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
