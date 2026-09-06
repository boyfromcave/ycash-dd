#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Unit tests for the coordinator's price-source layer (plan §5 step 1, D22).

No node, no network: every HTTP fetch is replaced by a table of canned
replies. The CoinGecko and Nonkyc fixtures are trimmed copies of real
replies captured on 2026-09-05, the SafeTrade (Peatio) fixture likewise.

Run:  python3 -m unittest contrib/yellowback/test_yellowback_fed.py
"""

import copy
import importlib.util
import os
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("yellowback_fed", os.path.join(HERE, "yellowback_fed.py"))
fed = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fed)

MICRO = fed.MICRO
NOW = 1_788_673_100.0

CG_SIMPLE = {"ycash": {"usd": 0.432007, "btc": 5.41e-06, "last_updated_at": 1788673030}}

CG_TICKERS = {"name": "Ycash", "tickers": [
    {"base": "YEC", "target": "USDT", "market": {"name": "SafeTrade", "identifier": "safe_trade"},
     "last": 0.43, "volume": 16077.362, "converted_last": {"btc": 5.38e-06, "usd": 0.429996},
     "bid_ask_spread_percentage": 14.891806, "last_traded_at": "2026-09-06T05:34:00+00:00",
     "is_anomaly": False, "is_stale": False},
    {"base": "YEC", "target": "USDT", "market": {"name": "Nonkyc.io", "identifier": "nonkyc_io"},
     "last": 0.4374, "volume": 6835.5782, "converted_last": {"btc": 5.47e-06, "usd": 0.437398},
     "bid_ask_spread_percentage": 1.281171, "last_traded_at": "2026-09-06T05:35:24Z",
     "is_anomaly": False, "is_stale": False},
    {"base": "YEC", "target": "BTC", "market": {"name": "Nonkyc.io", "identifier": "nonkyc_io"},
     "last": 5.356e-06, "volume": 1022.5283, "converted_last": {"btc": 5.356e-06, "usd": 0.428},
     "bid_ask_spread_percentage": 0.354742, "last_traded_at": "2026-09-06T05:35:24+00:00",
     "is_anomaly": False, "is_stale": False},
]}

NONKYC = {"symbol": "YEC/USDT", "lastPrice": "0.4426", "lastPriceNumber": 0.4426,
          "bestBidNumber": 0.4425, "bestAskNumber": 0.4466, "spreadPercent": "0.918",
          "spreadPercentNumber": 0.918, "lastTradeAt": 1788673068806, "updatedAt": 1788673069029}

PEATIO = {"id": "yecusdt", "name": "YEC/USDT", "base_unit": "yec", "quote_unit": "usdt", "avg_price": "0.41406",
          "high": "0.43", "last": "0.43", "low": "0.38", "open": "0.4", "price_change_percent": "+7.50%",
          "volume": "6613.51997911", "amount": "16041.975"}

NONKYC_BTC = {"symbol": "YEC/BTC", "lastPriceNumber": 5.57e-06, "bestBidNumber": 5.51e-06, "bestAskNumber": 5.586e-06,
              "lastTradeAt": 1788673915214, "volumeNumber": 989.3724}
NONKYC_BTCUSDT = {"symbol": "BTC/USDT", "lastPriceNumber": 79990.24, "bestBidNumber": 79453.71, "bestAskNumber": 80200,
                  "lastTradeAt": 1788674017285}
CG_BTC = {"bitcoin": {"usd": 79993, "last_updated_at": 1788673920}}
KRAKEN = {"error": [], "result": {"XXBTZUSD": {"a": ["79997.30000", "1", "1.000"], "b": ["79997.20000", "1", "1.000"],
                                               "c": ["79997.30000", "0.00008394"]}}}
COINBASE = {"price": "79996.81", "bid": "79996.8", "ask": "79996.81", "time": "2026-09-06T05:53:57.841478937Z"}
BTC_REF_USD = sorted([79993, 79990.24, 79997.30, 79996.81])
BTC_REF_USD = (BTC_REF_USD[1] + BTC_REF_USD[2]) / 2      # median of four

BTC_SOURCES = [
    {"name": "coingecko-btc", "kind": "coingecko_simple", "coin": "bitcoin", "max_age": 900},
    {"name": "nonkyc-btc", "kind": "nonkyc_market", "symbol": "BTC_USDT", "max_age": 3600},
    {"name": "kraken", "kind": "kraken_ticker"},
    {"name": "coinbase", "kind": "coinbase_ticker", "max_age": 600},
]
BTC_TABLE = {
    "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin": CG_BTC,
    "https://api.nonkyc.io/api/v2/market/getbysymbol/BTC_USDT": NONKYC_BTCUSDT,
    "https://api.kraken.com/0/public/Ticker?pair=XBTUSD": KRAKEN,
    "https://api.exchange.coinbase.com/products/BTC-USD/ticker": COINBASE,
    "https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_BTC": NONKYC_BTC,
}


class FakeHTTP:
    """Maps URL -> JSON object or bytes or Exception."""

    def __init__(self, table):
        self.table = table
        self.calls = []

    def __call__(self, url, headers, timeout):
        self.calls.append((url, headers))
        for prefix, reply in self.table.items():
            if url.startswith(prefix):
                if isinstance(reply, Exception):
                    raise reply
                if isinstance(reply, bytes):
                    return reply
                return fed.json.dumps(reply).encode()
        raise OSError("no fixture for %s" % url)


def make_feed(sources, table, btc_sources=None, **settings):
    feed = fed.PriceFeed(sources, None, dict(fed.FEED_DEFAULTS, **settings), btc_sources)
    http = FakeHTTP(table)
    feed._http_get = http
    feed.http = http
    return feed


class ExtractTests(unittest.TestCase):
    def test_dotted_and_index(self):
        self.assertEqual(fed.extract(CG_SIMPLE, "ycash.usd"), 0.432007)
        self.assertEqual(fed.extract(CG_TICKERS, "tickers.1.target"), "USDT")

    def test_selector_is_order_independent(self):
        path = "tickers.[market.identifier=nonkyc_io,target=USDT].converted_last.usd"
        self.assertEqual(fed.extract(CG_TICKERS, path), 0.437398)
        reordered = copy.deepcopy(CG_TICKERS)
        reordered["tickers"].reverse()
        self.assertEqual(fed.extract(reordered, path), 0.437398)
        # the BTC pair of the same venue is a different element
        self.assertEqual(fed.extract(reordered, "tickers.[market.identifier=nonkyc_io,target=BTC].last"), 5.356e-06)

    def test_shape_errors(self):
        with self.assertRaises(fed.ShapeError):
            fed.extract(CG_SIMPLE, "ycash.eur")
        with self.assertRaises(fed.ShapeError):
            fed.extract(CG_TICKERS, "tickers.[market.identifier=binance,target=USDT].last")
        with self.assertRaises(fed.ShapeError):
            fed.extract(CG_TICKERS, "tickers.[market.identifier=nonkyc_io].converted_last.usd.deeper")
        with self.assertRaises(fed.ShapeError):
            fed.extract_number(CG_TICKERS, "tickers.0.market.name")

    def test_timestamps(self):
        self.assertEqual(fed.extract_timestamp(CG_SIMPLE, "ycash.last_updated_at", "s"), 1788673030)
        self.assertAlmostEqual(fed.extract_timestamp(NONKYC, "lastTradeAt", "ms"), 1788673068.806, places=3)
        iso_plus = fed.extract_timestamp(CG_TICKERS, "tickers.0.last_traded_at", "iso")
        iso_z = fed.extract_timestamp(CG_TICKERS, "tickers.1.last_traded_at", "iso")
        self.assertEqual(iso_z - iso_plus, 84)
        self.assertAlmostEqual(fed.extract_timestamp(COINBASE, "time", "iso") % 1, 0.841478, places=5)

    def test_spread(self):
        sel = "tickers.[market.identifier=safe_trade,target=USDT]"
        src = fed.normalize_source({"name": "x", "url": "u", "path": "p", "spread_path": sel + ".bid_ask_spread_percentage"})
        self.assertAlmostEqual(fed.spread_bps(CG_TICKERS, src), 1489.18, places=1)
        src = fed.normalize_source({"name": "x", "url": "u", "path": "p", "bid_path": "bestBidNumber", "ask_path": "bestAskNumber"})
        self.assertAlmostEqual(fed.spread_bps(NONKYC, src), 92.2, places=0)
        src = fed.normalize_source({"name": "x", "url": "u", "path": "p"})
        self.assertIsNone(fed.spread_bps(NONKYC, src))


class ConfigTests(unittest.TestCase):
    def test_presets_expand_and_override(self):
        s = fed.normalize_source({"name": "cg", "kind": "coingecko_simple"})
        self.assertIn("ids=ycash&vs_currencies=usd", s["url"])
        self.assertEqual((s["path"], s["timestamp_path"], s["venue"], s["quote"]), ("ycash.usd", "ycash.last_updated_at", "coingecko", "USD"))
        s = fed.normalize_source({"name": "cg-btc", "kind": "coingecko_simple", "vs": "btc"})
        self.assertEqual((s["path"], s["quote"]), ("ycash.btc", "BTC"))
        s = fed.normalize_source({"name": "st", "kind": "coingecko_ticker", "market": "safe_trade", "target": "USDT", "api_key": "k"})
        self.assertEqual(s["path"], "tickers.[market.identifier=safe_trade,target=USDT].converted_last.usd")
        self.assertEqual(s["venue"], "safe_trade")
        self.assertEqual(s["headers"], {"x-cg-demo-api-key": "k"})
        self.assertEqual(len(s["reject_paths"]), 2)
        s = fed.normalize_source({"name": "st-btc", "kind": "coingecko_ticker", "market": "safe_trade", "target": "BTC"})
        self.assertEqual((s["path"], s["quote"]), ("tickers.[market.identifier=safe_trade,target=BTC].last", "BTC"))
        s = fed.normalize_source({"name": "kr", "kind": "kraken_ticker"})
        self.assertEqual((s["url"], s["path"], s["bid_path"], s["venue"]), ("https://api.kraken.com/0/public/Ticker?pair=XBTUSD", "result.XXBTZUSD.c.0", "result.XXBTZUSD.b.0", "kraken"))
        s = fed.normalize_source({"name": "cb", "kind": "coinbase_ticker"})
        self.assertEqual((s["path"], s["timestamp_path"], s["timestamp_unit"]), ("price", "time", "iso"))
        with self.assertRaises(ValueError):                       # a BTC/USD reference must be USD-quoted
            fed.normalize_btc_sources([{"name": "x", "kind": "nonkyc_market", "symbol": "YEC_BTC"}])
        s = fed.normalize_source({"name": "nk", "kind": "nonkyc_market", "symbol": "YEC_BTC", "path": "bestBidNumber"})
        self.assertEqual((s["quote"], s["path"], s["timestamp_unit"]), ("BTC", "bestBidNumber", "ms"))
        s = fed.normalize_source({"name": "st", "kind": "peatio_ticker", "base_url": "https://mirror.example"})
        self.assertEqual(s["url"], "https://mirror.example/api/v2/trade/public/tickers/yecusdt")
        self.assertEqual((s["path"], s["quote"], s.get("timestamp_path")), ("last", "USD", None))
        with self.assertRaises(ValueError):                       # no timestamp in this API: max_age is refused
            fed.normalize_source({"name": "st", "kind": "peatio_ticker", "max_age": 60})
        s = fed.normalize_source({"name": "g", "url": "https://x", "path": "a.b", "venue": "v"})
        self.assertEqual((s["kind"], s["venue"], s["scale"]), ("generic", "v", 1))

    def test_rejects(self):
        bad = [
            {"name": "a"},                                                     # no url/path
            {"name": "a", "url": "u", "path": "p", "kind": "binance"},         # unknown kind
            {"name": "a", "url": "u", "path": "p", "quote": "EUR"},
            {"name": "a", "url": "u", "path": "p", "timestamp_unit": "ns", "timestamp_path": "t"},
            {"name": "a", "url": "u", "path": "p", "max_age": 60},             # needs timestamp_path
            {"name": "a", "url": "u", "path": "p", "max_spread_bps": 10},      # needs spread or bid/ask
            {"name": "a", "url": "u", "path": "p", "typo": 1},
            {"url": "u", "path": "p"},                                         # no name
        ]
        for b in bad:
            with self.assertRaises(ValueError, msg=repr(b)):
                fed.normalize_source(b)
        with self.assertRaises(ValueError):
            fed.normalize_sources([{"name": "a", "url": "u", "path": "p"}, {"name": "a", "url": "v", "path": "p"}])

    def test_feed_settings(self):
        st = fed.feed_settings({"min_sources": "2", "poll_seconds": 5})
        self.assertEqual((st["min_sources"], st["poll_seconds"], st["min_venues"]), (2, 5, 2))
        with self.assertRaises(ValueError):
            fed.feed_settings({"min_venues": 0})


THREE = [
    {"name": "coingecko", "kind": "coingecko_simple", "max_age": 900},
    {"name": "safetrade", "kind": "coingecko_ticker", "market": "safe_trade", "target": "USDT", "max_age": 3600},
    {"name": "nonkyc", "kind": "nonkyc_market", "max_age": 3600, "max_spread_bps": 500},
]
TABLE = {
    "https://api.coingecko.com/api/v3/simple/price?ids=ycash": CG_SIMPLE,
    "https://api.coingecko.com/api/v3/coins/ycash/tickers": CG_TICKERS,
    "https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_USDT": NONKYC,
}


class FeedTests(unittest.TestCase):
    def setUp(self):
        self._time = fed.time.time
        fed.time.time = lambda: NOW

    def tearDown(self):
        fed.time.time = self._time

    def test_three_sources_two_venues(self):
        feed = make_feed(THREE, TABLE)
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual([r["sources"][n]["state"] for n in ("coingecko", "safetrade", "nonkyc")], ["ok"] * 3)
        self.assertEqual((r["live_sources"], r["live_venues"]), (3, 3))
        expected = sorted([432007, 429996, 442600])[1]
        self.assertEqual(feed.median_micro_usd(), expected)
        self.assertAlmostEqual(r["sources"]["nonkyc"]["last_spread_bps"], 92.2, places=0)
        self.assertEqual(r["sources"]["coingecko"]["last_age_seconds"], NOW - 1788673030)

    def test_poll_interval_and_force(self):
        feed = make_feed(THREE, TABLE, poll_seconds=30)
        feed.poll()
        feed.poll()
        self.assertEqual(len(feed.http.calls), 3)
        feed.poll(force=True)
        self.assertEqual(len(feed.http.calls), 6)

    def test_reordered_tickers_still_resolve(self):
        table = dict(TABLE)
        reordered = copy.deepcopy(CG_TICKERS)
        reordered["tickers"].reverse()
        table["https://api.coingecko.com/api/v3/coins/ycash/tickers"] = reordered
        feed = make_feed(THREE, table)
        feed.poll(force=True)
        self.assertEqual(feed.report()["sources"]["safetrade"]["last_price_micro_usd"], 429996)

    def test_stale_source_is_dropped(self):
        table = dict(TABLE)
        old = copy.deepcopy(NONKYC)
        old["lastTradeAt"] = int((NOW - 2 * 3600) * 1000)
        table["https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_USDT"] = old
        feed = make_feed(THREE, table)
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual(r["sources"]["nonkyc"]["state"], "stale")
        self.assertEqual(r["live_sources"], 2)
        self.assertIsNone(feed.median_micro_usd())               # below min_sources = 3
        feed2 = make_feed(THREE, table, min_sources=2)
        feed2.poll(force=True)
        self.assertEqual(feed2.median_micro_usd(), (432007 + 429996) // 2 + ((432007 + 429996) % 2))

    def test_spread_guard(self):
        srcs = copy.deepcopy(THREE)
        srcs[1]["max_spread_bps"] = 1000                          # SafeTrade's fixture spread is 1489 bps
        feed = make_feed(srcs, TABLE, min_sources=2)
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual(r["sources"]["safetrade"]["state"], "spread")
        self.assertEqual(r["live_sources"], 2)

    def test_flagged_ticker_is_rejected(self):
        table = dict(TABLE)
        flagged = copy.deepcopy(CG_TICKERS)
        flagged["tickers"][0]["is_anomaly"] = True
        table["https://api.coingecko.com/api/v3/coins/ycash/tickers"] = flagged
        feed = make_feed(THREE, table, min_sources=2)
        feed.poll(force=True)
        self.assertEqual(feed.report()["sources"]["safetrade"]["state"], "flagged")

    def test_shape_change_is_classified(self):
        table = dict(TABLE)
        table["https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_USDT"] = {"symbol": "YEC/USDT", "last": "0.44"}
        table["https://api.coingecko.com/api/v3/simple/price?ids=ycash"] = b"<!DOCTYPE html><title>Attention Required! | Cloudflare</title>"
        feed = make_feed(THREE, table)
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual(r["sources"]["nonkyc"]["state"], "shape")
        self.assertIn("lastPriceNumber", r["sources"]["nonkyc"]["last_error"])
        self.assertEqual(r["sources"]["coingecko"]["state"], "shape")
        self.assertIn("not JSON", r["sources"]["coingecko"]["last_error"])
        self.assertIsNone(feed.median_micro_usd())

    def test_fetch_failure_and_recovery(self):
        table = dict(TABLE)
        table["https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_USDT"] = OSError("connection refused")
        feed = make_feed(THREE, table)
        feed.poll(force=True)
        self.assertEqual(feed.report()["sources"]["nonkyc"]["state"], "fetch")
        feed.http.table = dict(TABLE)
        feed.poll(force=True)
        h = feed.report()["sources"]["nonkyc"]
        self.assertEqual((h["state"], h["ok"], h["failed"]), ("ok", 1, 1))

    def test_min_venues(self):
        # three sources that all name the same venue: enough sources, not enough venues
        srcs = [dict(s, venue="one") for s in THREE]
        feed = make_feed(srcs, TABLE)
        feed.poll(force=True)
        self.assertEqual(feed.report()["live_venues"], 1)
        self.assertIsNone(feed.median_micro_usd())
        feed = make_feed(srcs, TABLE, min_venues=1)
        feed.poll(force=True)
        self.assertIsNotNone(feed.median_micro_usd())

    def test_outlier_filter(self):
        table = dict(TABLE)
        wild = copy.deepcopy(NONKYC)
        wild["lastPriceNumber"] = 0.60                              # +39 % against the others
        table["https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_USDT"] = wild
        srcs = copy.deepcopy(THREE)
        srcs[2].pop("max_spread_bps")
        feed = make_feed(srcs, table, min_sources=2)
        feed.poll(force=True)
        self.assertEqual(feed.median_micro_usd(), (432007 + 429996 + 1) // 2)

    def test_safetrade_direct(self):
        srcs = THREE + [{"name": "safetrade-direct", "kind": "peatio_ticker", "market": "yecusdt"}]
        table = dict(TABLE)
        table["https://safe.trade/api/v2/trade/public/tickers/yecusdt"] = PEATIO
        feed = make_feed(srcs, table)
        feed.poll(force=True)
        h = feed.report()["sources"]["safetrade-direct"]
        self.assertEqual((h["state"], h["last_price_micro_usd"], h["last_age_seconds"], h["last_spread_bps"]), ("ok", 430000, None, None))
        self.assertEqual(feed.report()["live_venues"], 3)

    def test_btc_pairs_convert_with_reference_median(self):
        srcs = THREE + [
            {"name": "nonkyc-btc-pair", "kind": "nonkyc_market", "symbol": "YEC_BTC", "max_age": 3600, "max_spread_bps": 300},
            {"name": "nonkyc-btc-via-coingecko", "kind": "coingecko_ticker", "market": "nonkyc_io", "target": "BTC", "max_age": 3600},
        ]
        feed = make_feed(srcs, dict(TABLE, **BTC_TABLE), btc_sources=BTC_SOURCES)
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual(r["btc_usd"]["live"], 4)
        self.assertAlmostEqual(r["btc_usd"]["reference_usd"], BTC_REF_USD, places=6)
        self.assertEqual([h["state"] for h in r["btc_usd"]["sources"].values()], ["ok"] * 4)
        self.assertAlmostEqual(r["btc_usd"]["sources"]["kraken"]["last_spread_bps"], 0.0125, places=3)
        nk = r["sources"]["nonkyc-btc-pair"]
        self.assertEqual((nk["state"], nk["quote"]), ("ok", "BTC"))
        self.assertEqual(nk["last_price_micro_usd"], int(round(5.57e-06 * BTC_REF_USD * MICRO)))
        cg = r["sources"]["nonkyc-btc-via-coingecko"]
        self.assertEqual(cg["last_price_micro_usd"], int(round(5.356e-06 * BTC_REF_USD * MICRO)))
        self.assertEqual((r["live_sources"], r["live_venues"]), (5, 3))   # the BTC pairs share nonkyc_io
        self.assertIsNotNone(feed.median_micro_usd())

    def test_btc_pairs_dropped_without_reference(self):
        srcs = THREE + [{"name": "nonkyc-btc-pair", "kind": "nonkyc_market", "symbol": "YEC_BTC"}]
        table = dict(TABLE, **BTC_TABLE)
        table["https://api.kraken.com/0/public/Ticker?pair=XBTUSD"] = OSError("down")
        table["https://api.exchange.coinbase.com/products/BTC-USD/ticker"] = {"message": "NotFound"}
        table["https://api.nonkyc.io/api/v2/market/getbysymbol/BTC_USDT"] = OSError("down")
        feed = make_feed(srcs, table, btc_sources=BTC_SOURCES)          # one of four references live, need 2
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual(r["btc_usd"]["live"], 1)
        self.assertIsNone(r["btc_usd"]["reference_usd"])
        self.assertEqual(r["btc_usd"]["sources"]["coinbase"]["state"], "shape")
        self.assertEqual(r["sources"]["nonkyc-btc-pair"]["state"], "btcref")
        self.assertEqual(r["live_sources"], 3)                           # the USD sources are unaffected
        feed = make_feed(srcs, table, btc_sources=BTC_SOURCES, min_btc_sources=1)
        feed.poll(force=True)
        r = feed.report()
        self.assertEqual(r["sources"]["nonkyc-btc-pair"]["state"], "ok")
        self.assertEqual(r["sources"]["nonkyc-btc-pair"]["last_price_micro_usd"], int(round(5.57e-06 * 79993 * MICRO)))

    def test_btc_references_not_fetched_without_btc_pairs(self):
        feed = make_feed(THREE, dict(TABLE, **BTC_TABLE), btc_sources=BTC_SOURCES)
        feed.poll(force=True)
        self.assertEqual(len(feed.http.calls), 3)
        self.assertEqual(feed.report()["btc_usd"]["live"], 0)

    def test_twap_and_silence(self):
        feed = make_feed(THREE, TABLE, poll_seconds=0)
        feed.poll(force=True)
        fed.time.time = lambda: NOW + 100
        table = dict(TABLE)
        later = copy.deepcopy(NONKYC)
        later["lastPriceNumber"] = 0.4626
        later["lastTradeAt"] = int((NOW + 100) * 1000)
        feed.http.table = table
        table["https://api.nonkyc.io/api/v2/market/getbysymbol/YEC_USDT"] = later
        feed.poll(force=True)
        self.assertEqual(feed.report()["sources"]["nonkyc"]["twap_micro_usd"], (442600 + 462600) // 2)
        fed.time.time = lambda: NOW + 100 + fed.SOURCE_SILENCE_SECONDS + 1
        self.assertEqual(feed.report()["live_sources"], 0)

    def test_mock_mode_unchanged(self):
        path = os.path.join(HERE, ".test-mock-price")
        try:
            with open(path, "w") as f:
                f.write("50")
            feed = fed.PriceFeed([], path, dict(fed.FEED_DEFAULTS))
            feed.poll()
            self.assertEqual(feed.median_micro_usd(), 50 * MICRO)
            with open(path, "w") as f:
                f.write("75")
            feed.poll()
            self.assertEqual(feed.median_micro_usd(), 75 * MICRO)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
