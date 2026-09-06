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
  * price rounds: fetch YEC/USD from every configured source (presets for
    the known APIs, a generic URL + JSON path otherwise; freshness and spread
    guards per source; YEC/BTC pairs converted with the median of the
    configured BTC/USD references), TWAP per source, drop silent sources,
    10 % outlier filter, median over >= min_sources sources from
    >= min_venues venues, +/-10 % move clamp against the last on-chain price; the proposer of the round builds the PRICE
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
  yellowback_fed.py sources --config fed.toml   # fetch every source once and show what resolved
"""

import argparse
import base64
import datetime
import json
import logging
import os
import re
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
# Price sources (plan §5 step 1, D22)
#
# Every source is a [[sources]] table in the operator's TOML. `kind` names a
# preset that fills in the URL and the JSON paths for a known API; the operator
# may override any field, and kind = "generic" (the default) takes the fields
# verbatim. A venue changing the shape of its reply is therefore a config edit
# (or a preset edit here), never a node release. A source that stops resolving
# is dropped from the median and reported in /status and `sources`, and the
# federation's own checks (outlier filter, peer tolerance, move clamp, fail
# closed below min_sources) bound what a wrong number can do.
#
# Paths are dotted. A segment `[field=value,field2=value2]` selects the element
# of a list whose fields equal the given values, so a list whose order is not
# stable (CoinGecko's tickers) is still addressable; a bare integer indexes.
#
# BTC-quoted pairs (quote = "BTC") are converted with the median of the
# [[btc_usd_sources]] fetched in the same poll; below min_btc_sources live
# references every BTC-quoted source is dropped ("btcref"), never guessed.

FEED_DEFAULTS = {
    "poll_seconds": 30,          # fetch cadence; CoinGecko's public API allows ~30 calls/min and caches 30 s
    "twap_seconds": TWAP_SECONDS,
    "silence_seconds": SOURCE_SILENCE_SECONDS,
    "outlier_bps": OUTLIER_BPS,
    "min_sources": MIN_SOURCES,  # distinct live sources after the outlier filter
    "min_venues": 2,             # distinct `venue` values among them (CoinGecko aggregates the same venues)
    "min_btc_sources": 2,        # live BTC/USD references needed before a BTC-quoted pair is converted
    "fetch_timeout": 15,
}
FEED_KEYS = tuple(FEED_DEFAULTS)

SOURCE_FIELDS = {
    "name", "kind", "venue", "url", "path", "quote", "scale",
    "timestamp_path", "timestamp_unit", "max_age",
    "spread_path", "spread_unit", "bid_path", "ask_path", "max_spread_bps",
    "reject_paths", "headers",
    # preset parameters
    "coin", "vs", "market", "target", "symbol", "base_url", "api_key", "pair", "key", "product",
}


def _quote_for(symbol):
    return "BTC" if symbol.upper().endswith("BTC") else "USD"


def _preset_coingecko_simple(s):
    # Verified 2026-09-05: {"ycash":{"usd":0.432007,"last_updated_at":1788673030}}
    coin, vs = s.get("coin", "ycash"), s.get("vs", "usd").lower()
    d = {
        "venue": "coingecko",
        "url": "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=%s&include_last_updated_at=true" % (coin, vs),
        "path": "%s.%s" % (coin, vs),
        "quote": "BTC" if vs == "btc" else "USD",
        "timestamp_path": "%s.last_updated_at" % coin,
        "timestamp_unit": "s",
    }
    if s.get("api_key"):
        d["headers"] = {"x-cg-demo-api-key": s["api_key"]}
    return d


def _preset_coingecko_ticker(s):
    # Verified 2026-09-05: tickers[] with market.identifier ("safe_trade", "nonkyc_io"), target
    # ("USDT", "USDC", "BTC"), converted_last.usd, bid_ask_spread_percentage, last_traded_at (ISO),
    # is_stale, is_anomaly. The list order is not stable: select by fields.
    # target = "BTC" reads the venue's own last (BTC) and converts with our BTC/USD reference,
    # not CoinGecko's converted_last.
    coin = s.get("coin", "ycash")
    market, target = s.get("market", "safe_trade"), s.get("target", "USDT")
    sel = "tickers.[market.identifier=%s,target=%s]" % (market, target)
    btc = target.upper() == "BTC"
    d = {
        "venue": market,
        "url": "https://api.coingecko.com/api/v3/coins/%s/tickers" % coin,
        "path": sel + (".last" if btc else ".converted_last.usd"),
        "quote": "BTC" if btc else "USD",
        "timestamp_path": sel + ".last_traded_at",
        "timestamp_unit": "iso",
        "spread_path": sel + ".bid_ask_spread_percentage",
        "spread_unit": "percent",
        "reject_paths": [sel + ".is_stale", sel + ".is_anomaly"],
    }
    if s.get("api_key"):
        d["headers"] = {"x-cg-demo-api-key": s["api_key"]}
    return d


def _preset_nonkyc_market(s):
    # Verified 2026-09-05: lastPriceNumber, bestBidNumber, bestAskNumber, spreadPercentNumber,
    # lastTradeAt (ms since epoch).
    symbol = s.get("symbol", "YEC_USDT")
    return {
        "venue": "nonkyc_io",       # CoinGecko's market identifier, so both readings of the venue count once
        "url": "%s/api/v2/market/getbysymbol/%s" % (s.get("base_url", "https://api.nonkyc.io"), symbol),
        "path": "lastPriceNumber",
        "quote": _quote_for(symbol),
        "timestamp_path": "lastTradeAt",
        "timestamp_unit": "ms",
        "bid_path": "bestBidNumber",
        "ask_path": "bestAskNumber",
    }


def _preset_peatio_ticker(s):
    # SafeTrade (Peatio/OpenDAX API). Verified 2026-09-05 with this client's headers (Cloudflare
    # answered curl and a browser user agent with 403; `yellowback_fed.py sources` from the operator
    # host is the check): {"id":"yecusdt","base_unit":"yec","quote_unit":"usdt","last":"0.43",
    # "avg_price","high","low","open","volume","amount","price_change_percent"}. No timestamp and no
    # bid/ask: freshness and spread for this venue come from the coingecko_ticker preset instead.
    market = s.get("market", "yecusdt")
    return {
        "venue": "safe_trade",
        "url": "%s/api/v2/trade/public/tickers/%s" % (s.get("base_url", "https://safe.trade"), market),
        "path": "last",
        "quote": _quote_for(market),
    }


def _preset_kraken_ticker(s):
    # Verified 2026-09-05: {"error":[],"result":{"XXBTZUSD":{"a":[ask,..],"b":[bid,..],"c":[last,vol],..}}}.
    # Kraken keys the result by its internal pair name (XBTUSD -> XXBTZUSD); `key` overrides it.
    pair = s.get("pair", "XBTUSD")
    key = s.get("key", "XXBTZUSD" if pair == "XBTUSD" else pair)
    return {
        "venue": "kraken",
        "url": "%s/0/public/Ticker?pair=%s" % (s.get("base_url", "https://api.kraken.com"), pair),
        "path": "result.%s.c.0" % key,
        "quote": "USD",
        "bid_path": "result.%s.b.0" % key,
        "ask_path": "result.%s.a.0" % key,
    }


def _preset_coinbase_ticker(s):
    # Verified 2026-09-05: {"price","bid","ask","time":"2026-09-06T05:53:57.841478937Z",...}.
    product = s.get("product", "BTC-USD")
    return {
        "venue": "coinbase",
        "url": "%s/products/%s/ticker" % (s.get("base_url", "https://api.exchange.coinbase.com"), product),
        "path": "price",
        "quote": "USD",
        "timestamp_path": "time",
        "timestamp_unit": "iso",
        "bid_path": "bid",
        "ask_path": "ask",
    }


PRESETS = {
    "generic": lambda s: {},
    "kraken_ticker": _preset_kraken_ticker,
    "coinbase_ticker": _preset_coinbase_ticker,
    "coingecko_simple": _preset_coingecko_simple,
    "coingecko_ticker": _preset_coingecko_ticker,
    "nonkyc_market": _preset_nonkyc_market,
    "peatio_ticker": _preset_peatio_ticker,
}


def normalize_source(src):
    """Expand a [[sources]] table through its preset and validate it. Raises ValueError."""
    if not isinstance(src, dict) or not src.get("name"):
        raise ValueError("every source needs a name")
    name = src["name"]
    unknown = set(src) - SOURCE_FIELDS
    if unknown:
        raise ValueError("source %s: unknown field(s) %s" % (name, ", ".join(sorted(unknown))))
    kind = src.get("kind", "generic")
    if kind not in PRESETS:
        raise ValueError("source %s: unknown kind %r (known: %s)" % (name, kind, ", ".join(sorted(PRESETS))))
    out = dict(PRESETS[kind](src))
    out.update({k: v for k, v in src.items() if k in SOURCE_FIELDS})
    out["kind"] = kind
    out.setdefault("venue", name)
    out.setdefault("quote", "USD")
    out.setdefault("scale", 1)
    out.setdefault("timestamp_unit", "s")
    out.setdefault("spread_unit", "percent")
    out.setdefault("headers", {})
    out.setdefault("reject_paths", [])
    if not out.get("url") or not out.get("path"):
        raise ValueError("source %s: url and path are required (kind %s)" % (name, kind))
    if out["quote"] not in ("USD", "BTC"):
        raise ValueError("source %s: quote must be USD or BTC" % name)
    if out["timestamp_unit"] not in ("s", "ms", "iso"):
        raise ValueError("source %s: timestamp_unit must be s, ms or iso" % name)
    if out["spread_unit"] not in ("percent", "bps", "ratio"):
        raise ValueError("source %s: spread_unit must be percent, bps or ratio" % name)
    if out.get("max_age") is not None and not out.get("timestamp_path"):
        raise ValueError("source %s: max_age needs timestamp_path" % name)
    if out.get("max_spread_bps") is not None and not (out.get("spread_path") or (out.get("bid_path") and out.get("ask_path"))):
        raise ValueError("source %s: max_spread_bps needs spread_path or bid_path+ask_path" % name)
    return out


def normalize_sources(sources, what="sources"):
    out = [normalize_source(s) for s in sources or []]
    names = [s["name"] for s in out]
    dups = sorted({n for n in names if names.count(n) > 1})
    if dups:
        raise ValueError("duplicate %s name(s): %s" % (what, ", ".join(dups)))
    return out


def normalize_btc_sources(sources):
    out = normalize_sources(sources, "btc_usd_sources")
    for s in out:
        if s["quote"] != "USD":
            raise ValueError("btc_usd_sources %s: must be USD-quoted" % s["name"])
    return out


def feed_settings(cfg):
    st = dict(FEED_DEFAULTS)
    for k in FEED_KEYS:
        if cfg.get(k) is not None:
            st[k] = type(FEED_DEFAULTS[k])(cfg[k])
    if st["min_sources"] < 1 or st["min_venues"] < 1 or st["min_btc_sources"] < 1:
        raise ValueError("min_sources, min_venues and min_btc_sources must be >= 1")
    return st


class ShapeError(Exception):
    """The reply parsed as JSON but the configured path did not resolve."""


def _parse_selector(segment):
    body = segment[1:-1]
    conds = []
    for part in body.split(","):
        if "=" not in part:
            raise ValueError("bad selector %r" % segment)
        k, v = part.split("=", 1)
        conds.append((k.strip(), v.strip()))
    return conds


def _walk(obj, dotted):
    for part in dotted.split("."):
        if isinstance(obj, list):
            obj = obj[int(part)]
        else:
            obj = obj[part]
    return obj


def split_path(path):
    """Split a dotted path into segments; dots inside a [selector] do not split."""
    segs, cur, depth = [], "", 0
    for ch in path:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "." and depth == 0:
            segs.append(cur)
            cur = ""
        else:
            cur += ch
    segs.append(cur)
    if depth != 0:
        raise ShapeError("unbalanced brackets in path %s" % path)
    return segs


def extract(obj, path):
    """Resolve a dotted path with list selectors. Raises ShapeError when it does not resolve."""
    try:
        for seg in split_path(path):
            if seg.startswith("[") and seg.endswith("]"):
                if not isinstance(obj, list):
                    raise ShapeError("selector %s applied to a non-list" % seg)
                conds = _parse_selector(seg)
                match = None
                for el in obj:
                    try:
                        if all(str(_walk(el, k)) == v for k, v in conds):
                            match = el
                            break
                    except (KeyError, IndexError, TypeError, ValueError):
                        continue
                if match is None:
                    raise ShapeError("no list element matches %s" % seg)
                obj = match
            elif isinstance(obj, list):
                obj = obj[int(seg)]
            elif isinstance(obj, dict):
                obj = obj[seg]
            else:
                raise ShapeError("cannot descend into %r at %s" % (type(obj).__name__, seg))
    except ShapeError:
        raise
    except (KeyError, IndexError, TypeError, ValueError) as e:
        raise ShapeError("%s at path %s" % (e.__class__.__name__, path))
    return obj


def extract_number(obj, path):
    v = extract(obj, path)
    try:
        return float(v)
    except (TypeError, ValueError):
        raise ShapeError("value at %s is not a number: %r" % (path, v))


def extract_timestamp(obj, path, unit):
    v = extract(obj, path)
    try:
        if unit == "iso":
            s = str(v)
            if s.endswith("Z"):
                s = s[:-1] + "+00:00"
            s = re.sub(r"(\.\d{6})\d+", r"\1", s)   # fromisoformat takes at most microseconds
            dt = datetime.datetime.fromisoformat(s)
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=datetime.timezone.utc)
            return dt.timestamp()
        f = float(v)
        return f / 1000.0 if unit == "ms" else f
    except (TypeError, ValueError) as e:
        raise ShapeError("timestamp at %s unreadable (%s): %r" % (path, e, v))


def spread_bps(obj, src):
    """Spread in basis points from spread_path or bid/ask, or None when the source has neither."""
    if src.get("spread_path"):
        v = extract_number(obj, src["spread_path"])
        unit = src["spread_unit"]
        return v * 100 if unit == "percent" else v * 10000 if unit == "ratio" else v
    if src.get("bid_path") and src.get("ask_path"):
        bid, ask = extract_number(obj, src["bid_path"]), extract_number(obj, src["ask_path"])
        mid = (bid + ask) / 2
        if mid <= 0:
            raise ShapeError("bid/ask not positive")
        return (ask - bid) / mid * 10000
    return None


class PriceFeed:
    """TWAP per source, freshness/spread guards, silence drop, outlier filter, median (plan §5 step 1, A16, D22)."""

    def __init__(self, sources, mock_file=None, settings=None, btc_sources=None):
        self.sources = normalize_sources(sources)
        self.btc_sources = normalize_btc_sources(btc_sources)
        self.settings = dict(FEED_DEFAULTS, **(settings or {}))
        self.mock_file = mock_file
        self.samples = {}               # name -> [(t, micro_usd)]
        self.health = self._fresh_health(self.sources)
        self.btc_health = self._fresh_health(self.btc_sources)
        self.btc_reference = {"usd": None, "live": 0, "at": None}
        self.last_poll = 0.0
        self.lock = threading.Lock()

    @staticmethod
    def _fresh_health(sources):
        return {s["name"]: {"venue": s["venue"], "kind": s["kind"], "quote": s["quote"], "state": "never",
                            "ok": 0, "failed": 0, "last_ok": None, "last_error": None,
                            "last_price_micro_usd": None, "last_age_seconds": None,
                            "last_spread_bps": None} for s in sources}

    # ---- fetching
    @staticmethod
    def _http_get(url, headers, timeout):
        req = urllib.request.Request(url, headers=dict({"User-Agent": "yellowback-fed/1", "Accept": "application/json"}, **headers))
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read()

    def _fetch_json(self, url, headers):
        try:
            raw = self._http_get(url, headers, self.settings["fetch_timeout"])
        except Exception as e:
            raise RuntimeError("fetch: %s" % e)
        try:
            return json.loads(raw.decode())
        except (UnicodeDecodeError, ValueError):
            head = raw[:60].decode("utf-8", "replace").strip()
            raise ShapeError("reply is not JSON (starts %r)" % head)

    def _sample(self, src, now, btc_median):
        """One fetch of a source. Returns (micro_usd, age_seconds, spread_bps); raises on any refusal."""
        obj = self._fetch_json(src["url"], src["headers"])
        for p in src["reject_paths"]:
            if bool(extract(obj, p)):
                raise RuntimeError("flagged: %s is true" % p)
        price = extract_number(obj, src["path"]) * float(src["scale"])
        if price <= 0:
            raise ShapeError("price not positive: %r" % price)
        age = None
        if src.get("timestamp_path"):
            age = now - extract_timestamp(obj, src["timestamp_path"], src["timestamp_unit"])
            if src.get("max_age") is not None and age > float(src["max_age"]):
                raise RuntimeError("stale: last trade %.0f s ago > max_age %s" % (age, src["max_age"]))
        spread = spread_bps(obj, src)
        if spread is not None and src.get("max_spread_bps") is not None and spread > float(src["max_spread_bps"]):
            raise RuntimeError("spread: %.0f bps > max_spread_bps %s" % (spread, src["max_spread_bps"]))
        if src["quote"] == "BTC":
            if btc_median is None:
                raise RuntimeError("btcref: no BTC/USD reference (%d live, need %d)" % (self.btc_reference["live"], self.settings["min_btc_sources"]))
            price *= btc_median
        return int(round(price * MICRO)), age, spread

    def _record(self, name, ok, error=None, price=None, age=None, spread=None, now=None, health=None):
        h = (self.health if health is None else health)[name]
        prev = h["state"]
        if ok:
            h.update(ok=h["ok"] + 1, state="ok", last_ok=now, last_error=None,
                     last_price_micro_usd=price, last_age_seconds=age, last_spread_bps=spread)
            if prev not in ("ok", "never"):
                LOG.warning("source %s recovered", name)
        else:
            state = "shape" if isinstance(error, ShapeError) else str(error).split(":", 1)[0] if isinstance(error, RuntimeError) else "error"
            h.update(failed=h["failed"] + 1, state=state, last_error=str(error))
            msg = "source %s dropped (%s): %s" % (name, state, error)
            if state == "shape":
                msg += " -- reply parsed but the configured path did not resolve: did the API change shape? Check with `yellowback_fed.py sources`"
            (LOG.warning if prev != state else LOG.debug)(msg)

    def _btc_reference_usd(self, now):
        """Median of the live BTC/USD references, or None below min_btc_sources. Records their health."""
        values = []
        for s in self.btc_sources:
            try:
                micro, age, spread = self._sample(s, now, None)
            except Exception as e:
                self._record(s["name"], False, error=e, health=self.btc_health)
                continue
            self._record(s["name"], True, price=micro, age=age, spread=spread, now=now, health=self.btc_health)
            values.append(micro)
        usd = statistics.median(values) / MICRO if len(values) >= self.settings["min_btc_sources"] else None
        self.btc_reference = {"usd": usd, "live": len(values), "at": now}
        return usd

    def poll(self, force=False):
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
        if not force and now - self.last_poll < self.settings["poll_seconds"]:
            return
        self.last_poll = now
        btc_median = self._btc_reference_usd(now) if any(s["quote"] == "BTC" for s in self.sources) else None
        for s in self.sources:
            try:
                micro, age, spread = self._sample(s, now, btc_median)
            except Exception as e:
                self._record(s["name"], False, error=e)
                continue
            with self.lock:
                self.samples.setdefault(s["name"], []).append((now, micro))
            self._record(s["name"], True, price=micro, age=age, spread=spread, now=now)

    # ---- aggregation
    def _live_twaps(self, now):
        """(name, venue, twap) for every source with fresh samples."""
        venues = {s["name"]: s["venue"] for s in self.sources}
        out = []
        with self.lock:
            for name, samples in self.samples.items():
                samples[:] = [s for s in samples if now - s[0] <= self.settings["twap_seconds"]]
                if not samples or now - samples[-1][0] > self.settings["silence_seconds"]:
                    continue
                out.append((name, venues.get(name, name), sum(v for _, v in samples) / len(samples)))
        return out

    def median_micro_usd(self):
        """Median of the per-source TWAPs, or None below min_sources / min_venues (1 source in mock mode)."""
        now = time.time()
        if self.mock_file:
            with self.lock:
                # Test mode: the file is the whole market; no averaging, so a test's price change is immediate.
                s = self.samples.get("mock")
                return s[-1][1] if s else None
        live = self._live_twaps(now)
        if len(live) < self.settings["min_sources"]:
            return None
        med = statistics.median(t for _, _, t in live)
        kept = [(n, v, t) for n, v, t in live if abs(t - med) * 10000 <= self.settings["outlier_bps"] * med]
        if len(kept) < self.settings["min_sources"] or len({v for _, v, _ in kept}) < self.settings["min_venues"]:
            return None
        return int(round(statistics.median(t for _, _, t in kept)))

    def report(self):
        """Per-source health plus the aggregate, for /status and the `sources` command."""
        now = time.time()
        live = {n: t for n, _, t in self._live_twaps(now)}
        srcs = {}
        for name, h in self.health.items():
            d = dict(h)
            d["live"] = name in live
            d["twap_micro_usd"] = int(round(live[name])) if name in live else None
            srcs[name] = d
        return {"settings": dict(self.settings), "sources": srcs,
                "live_sources": len(live), "live_venues": len({v for _, v, _ in self._live_twaps(now)}),
                "btc_usd": {"reference_usd": self.btc_reference["usd"], "live": self.btc_reference["live"],
                            "sources": {n: dict(h) for n, h in self.btc_health.items()}},
                "median_micro_usd": self.median_micro_usd()}


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
        self.feed = PriceFeed(cfg.get("sources", []), cfg.get("mock_price"), feed_settings(cfg), cfg.get("btc_usd_sources", []))
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
                st["feed"] = c.feed.report()
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
    for key in ("rpc_url", "id", "listen", "mock_price", "round_blocks") + FEED_KEYS:
        v = getattr(args, key, None)
        if v is not None:
            cfg[key] = v
    if args.peers:
        cfg["peers"] = [p.strip() for p in args.peers.split(",") if p.strip()]
    if args.insecure_localhost:
        cfg["insecure_localhost"] = True
    if args.command != "sources":
        for req in ("rpc_url", "id"):
            if req not in cfg:
                raise SystemExit("missing %s" % req)
    try:
        normalize_sources(cfg.get("sources", []))
        normalize_btc_sources(cfg.get("btc_usd_sources", []))
        feed_settings(cfg)
    except ValueError as e:
        raise SystemExit("bad source configuration: %s" % e)
    return cfg


def _print_sources(title, table):
    if not table:
        return
    w = max(len(n) for n in table)
    print(title)
    print("  %-*s  %-12s  %-16s  %-7s  %-5s  %12s  %8s  %9s  %s" % (w, "source", "venue", "kind", "state", "quote", "micro_usd", "age_s", "spread", "detail"))
    for name, h in table.items():
        ok = h["state"] == "ok"
        print("  %-*s  %-12s  %-16s  %-7s  %-5s  %12s  %8s  %9s  %s" % (
            w, name, h["venue"], h["kind"], h["state"], h["quote"],
            "" if not ok or h["last_price_micro_usd"] is None else h["last_price_micro_usd"],
            "" if not ok or h["last_age_seconds"] is None else "%.0f" % h["last_age_seconds"],
            "" if not ok or h["last_spread_bps"] is None else "%.0f bps" % h["last_spread_bps"],
            h["last_error"] or ""))


def sources_command(cfg):
    """Fetch every configured source once and print what resolved; exit 1 if no price results."""
    feed = PriceFeed(cfg.get("sources", []), None, feed_settings(cfg), cfg.get("btc_usd_sources", []))
    if not feed.sources:
        print("no [[sources]] configured", file=sys.stderr)
        return 1
    feed.poll(force=True)
    r = feed.report()
    st = r["settings"]
    if feed.btc_sources:
        _print_sources("BTC/USD references (for BTC-quoted pairs):", r["btc_usd"]["sources"])
        ref = r["btc_usd"]["reference_usd"]
        print("  reference: %s (%d live, need min_btc_sources=%d)" % (
            "none" if ref is None else "$%.2f" % ref, r["btc_usd"]["live"], st["min_btc_sources"]))
    _print_sources("YEC/USD sources:", r["sources"])
    print("live: %d source(s) from %d venue(s); need min_sources=%d, min_venues=%d; outlier_bps=%d" % (
        r["live_sources"], r["live_venues"], st["min_sources"], st["min_venues"], st["outlier_bps"]))
    if r["median_micro_usd"] is None:
        print("median: none (no price would be published)")
        return 1
    print("median: %d micro-USD ($%.6f)" % (r["median_micro_usd"], r["median_micro_usd"] / MICRO))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["serve", "rotate", "status", "sources"])
    ap.add_argument("--config")
    ap.add_argument("--rpc-url", dest="rpc_url", help="http://user:pass@127.0.0.1:port")
    ap.add_argument("--id", type=int)
    ap.add_argument("--listen", help="HOST:PORT for /cosign, /pricesign, /rotatesign, /status")
    ap.add_argument("--peers", help="comma-separated peer coordinator base URLs")
    ap.add_argument("--mock-price", dest="mock_price", help="TEST ONLY: read the USD price from this file")
    ap.add_argument("--insecure-localhost", dest="insecure_localhost", action="store_true", help="TEST ONLY: plain HTTP on 127.0.0.1")
    ap.add_argument("--round-blocks", dest="round_blocks", type=int)
    ap.add_argument("--poll-seconds", dest="poll_seconds", type=int, help="source fetch cadence (default %d)" % FEED_DEFAULTS["poll_seconds"])
    ap.add_argument("--min-sources", dest="min_sources", type=int, help="live sources needed for a price (default %d)" % FEED_DEFAULTS["min_sources"])
    ap.add_argument("--min-venues", dest="min_venues", type=int, help="distinct venues among them (default %d)" % FEED_DEFAULTS["min_venues"])
    ap.add_argument("--min-btc-sources", dest="min_btc_sources", type=int, help="live BTC/USD references needed to convert BTC pairs (default %d)" % FEED_DEFAULTS["min_btc_sources"])
    ap.add_argument("--new-roster", dest="new_roster", help="rotate: the new roster script hex")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s", stream=sys.stderr)
    cfg = load_config(args)
    if args.command == "sources":
        return sources_command(cfg)
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
            c.feed.poll(force=True)
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
