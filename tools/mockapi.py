#!/usr/bin/env python3
"""Faithful local replica of api.adsb.lol for testing the device offline.

The firmware calls client.setInsecure(), so a generated self-signed cert is
accepted; no CA setup is needed.

    tools/mockapi.py                 # https://0.0.0.0:8443
    tools/mockapi.py --port 9443

Two things make this a replica rather than a stub:

1. It ROUTES. Real paths are parsed and the lat/lon/radius are honoured against
   a synthetic fleet, so a wrong URL or a bad radius conversion actually fails
   instead of silently returning the same canned body.
2. It reproduces the quirks that have already caused real bugs here:
     - 403 "User-Agent too generic; include valid contact info." for anonymous UAs
     - 429 rate limiting (the real API limits at roughly 1 req/s)
     - Connection: keep-alive with Content-Length (this combination is what made
       a completed read look like a stall to the old /v2/mil scanner)
     - dbFlags omitted entirely when zero, never sent as 0
     - flight padded to 8 characters, so "no callsign" arrives as spaces
     - alt_baro as the STRING "ground", not a number, for surface aircraft

Every request is appended to logs/mock.requests.jsonl so tests can assert on
what the device actually asked for.

Fault mode is read per-request from logs/mock.mode:

    ok empty http403 http429 http500 malformed truncated slow hang garbage
    bigmil mil custom ratelimit uaenforce far emergency lifeguard priority
"""
import argparse, json, math, os, re, ssl, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.environ.get("LOGDIR", os.path.join(ROOT, "logs"))
MODE_FILE = os.path.join(LOGDIR, "mock.mode")
BODY_FILE = os.path.join(LOGDIR, "mock.body")
REQ_LOG = os.path.join(LOGDIR, "mock.requests.jsonl")
CERT = os.path.join(LOGDIR, "mock-cert.pem")
KEY = os.path.join(LOGDIR, "mock-key.pem")
SLOW_SECONDS = 40  # must exceed the firmware's HTTP_READ_TIMEOUT_MS (8s)
NM_PER_KM = 0.5399568

HOME_LAT, HOME_LON = 47.6506, -122.3694

# Synthetic fleet, positioned at known distances from HOME so radius handling is
# testable. Distances are filled in at import.
def _at(bearing_deg, km):
    """A point km away from HOME on the given bearing."""
    R = 6371.0
    b = math.radians(bearing_deg)
    lat1, lon1 = math.radians(HOME_LAT), math.radians(HOME_LON)
    lat2 = math.asin(math.sin(lat1) * math.cos(km / R) + math.cos(lat1) * math.sin(km / R) * math.cos(b))
    lon2 = lon1 + math.atan2(math.sin(b) * math.sin(km / R) * math.cos(lat1),
                             math.cos(km / R) - math.sin(lat1) * math.sin(lat2))
    return round(math.degrees(lat2), 6), round(math.degrees(lon2), 6)


def _ac(hex_, flight, reg, t, alt, km, bearing, **extra):
    lat, lon = _at(bearing, km)
    a = {"hex": hex_, "type": "adsb_icao", "flight": f"{flight:<8}" if flight else "        ",
         "r": reg, "t": t, "alt_baro": alt, "alt_geom": (alt + 25) if isinstance(alt, int) else 0,
         "gs": 250.0, "track": float(bearing), "baro_rate": 0, "squawk": "1200",
         "emergency": "none", "category": "A3", "lat": lat, "lon": lon,
         "seen_pos": 1.2, "seen": 1.0, "rssi": -12.3, "messages": 1234,
         "desc": "BOEING 737-800", "_km": km}
    a.update(extra)
    return a


# Ordered near -> far. The 3 km aircraft is inside the 10 km primary radius; the
# 60 km one is only reachable through the widened fallback search.
FLEET = [
    _ac("a11111", "MOCK101", "N101MK", "B738", 12000, 3.0, 10),
    _ac("a22222", "MOCK202", "N202MK", "C172", 4500, 8.0, 120, category="A1",
        desc="CESSNA 172"),
    _ac("a33333", "MOCK303", "N303MK", "A320", 24000, 45.0, 200, desc="AIRBUS A320"),
    _ac("ae0001", "RCH9001", "01-0001", "C17", 28000, 60.0, 300, dbFlags=1,
        desc="BOEING C-17A"),
]

_rate = {"hits": []}
_lock = threading.Lock()


def mode():
    try:
        with open(MODE_FILE) as f:
            return f.read().strip() or "ok"
    except OSError:
        return "ok"


def ensure_cert():
    if os.path.exists(CERT) and os.path.exists(KEY):
        return
    os.makedirs(LOGDIR, exist_ok=True)
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", KEY, "-out", CERT, "-days", "3650",
                    "-subj", "/CN=mock-adsb"], check=True, capture_output=True)
    print(f"generated self-signed cert in {LOGDIR}")


def haversine_km(lat1, lon1, lat2, lon2):
    R = 6371.0
    dlat, dlon = math.radians(lat2 - lat1), math.radians(lon2 - lon1)
    a = (math.sin(dlat / 2) ** 2
         + math.cos(math.radians(lat1)) * math.cos(math.radians(lat2)) * math.sin(dlon / 2) ** 2)
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def shape(a, lat, lon):
    """Emit a record the way the real API does: dbFlags omitted when zero,
    an internal _km helper stripped, and dst reported in nautical miles."""
    out = {k: v for k, v in a.items() if not k.startswith("_")}
    if not out.get("dbFlags"):
        out.pop("dbFlags", None)
    d = haversine_km(lat, lon, a["lat"], a["lon"])
    out["dst"] = round(d * NM_PER_KM, 3)
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "adsb-lol-mock"

    def log_message(self, fmt, *a):
        pass  # requests are recorded to REQ_LOG instead

    def _record(self, status, note=""):
        rec = {"ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "path": self.path, "ua": self.headers.get("User-Agent", ""),
               "mode": mode(), "status": status, "note": note}
        try:
            with open(REQ_LOG, "a") as f:
                f.write(json.dumps(rec) + "\n")
        except OSError:
            pass
        print(f"{rec['ts']} {status} {self.path}  ua={rec['ua'][:40]!r} {note}", flush=True)

    def _send(self, code, body, ctype="application/json", clen=None, note=""):
        raw = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        # Honour the client's Connection preference, as the real API does: it
        # answers keep-alive to a keep-alive request, and close to a close
        # request. Forcing keep-alive on a client that asked to close makes the
        # ESP32 read zero bytes (EmptyInput). The keep-alive path is still
        # reproduced for clients that ask for it — that is the case which once
        # made a fully-read body look like a stall.
        want = self.headers.get("Connection", "keep-alive").lower()
        self.send_header("Connection", "close" if "close" in want else "keep-alive")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(clen if clen is not None else len(raw)))
        self.end_headers()
        self.wfile.write(raw)
        try:
            self.wfile.flush()
        except OSError:
            pass
        if "close" in self.headers.get("Connection", "").lower():
            # Let a slow client drain a large body before the socket goes away.
            # An abrupt close can discard data still in flight on a constrained
            # TLS peer, which looks exactly like a parse failure at the client.
            if len(raw) > 2048:
                time.sleep(float(os.environ.get("MOCK_LINGER", "0.5")))
            self.close_connection = True
        self._record(code, note)

    # ---- quirk emulation -------------------------------------------------
    def _ua_rejected(self):
        """The real API 403s User-Agents with no contact info."""
        ua = self.headers.get("User-Agent", "")
        if not ua:
            return True
        generic = ("ESP32HTTPClient", "python-requests", "Go-http-client", "okhttp", "libwww")
        if any(g.lower() in ua.lower() for g in generic):
            return True
        # contact info means an email or a URL
        return not (re.search(r"[^@\s]+@[^@\s]+\.[a-z]+", ua) or "http" in ua.lower())

    def _rate_limited(self, limit_per_sec=1.0):
        now = time.time()
        with _lock:
            _rate["hits"] = [t for t in _rate["hits"] if now - t < 1.0]
            _rate["hits"].append(now)
            return len(_rate["hits"]) > limit_per_sec

    # ---- routing ---------------------------------------------------------
    def do_GET(self):
        m = mode()
        if m == "hang":
            time.sleep(600)
            return
        if m == "slow":
            time.sleep(SLOW_SECONDS)

        if m in ("uaenforce", "ok", "empty") and self._ua_rejected():
            return self._send(403, "User-Agent too generic; include valid contact info.\n",
                              "text/plain", note="UA rejected")
        if m == "ratelimit" and self._rate_limited():
            return self._send(429, "Too Many Requests\n", "text/plain", note="rate limited")

        forced = {"http403": (403, "User-Agent too generic; include valid contact info.\n"),
                  "http429": (429, "Too Many Requests\n"),
                  "http500": (500, "Internal Server Error\n")}
        if m in forced:
            code, body = forced[m]
            return self._send(code, body, "text/plain")
        if m == "malformed":
            return self._send(200, '{"ac":[{"hex":"a1b2c3",,,]}NOT JSON')
        if m == "garbage":
            return self._send(200, bytes(range(256)) * 8, "application/octet-stream")
        if m == "custom":
            try:
                with open(BODY_FILE, "rb") as f:
                    return self._send(200, f.read())
            except OSError:
                return self._send(500, "no body file\n", "text/plain")

        p = self.path.split("?")[0].rstrip("/")

        mm = re.fullmatch(r"/v2/(closest|point)/(-?[\d.]+)/(-?[\d.]+)/(\d+)", p)
        if mm:
            kind, lat, lon, radius_nm = mm.group(1), float(mm.group(2)), float(mm.group(3)), int(mm.group(4))
            radius_km = radius_nm / NM_PER_KM
            inside = [a for a in FLEET if haversine_km(lat, lon, a["lat"], a["lon"]) <= radius_km]
            inside.sort(key=lambda a: haversine_km(lat, lon, a["lat"], a["lon"]))
            if m == "empty":
                inside = []
            if m == "emergency":
                # The 8 km C172 squawks 7700 while the 3 km B738 is normal, so a
                # correct implementation shows the EMERGENCY one, not the nearest.
                inside = [dict(a, squawk="7700") if a["t"] == "C172" else a for a in inside]
            if m == "lifeguard":
                # No squawk equivalent exists: lifeguard lives only in the
                # ADS-B emergency/priority status field.
                inside = [dict(a, emergency="lifeguard") if a["t"] == "C172" else a
                          for a in inside]
            if m == "priority":
                # Two alerting aircraft at once: the NEARER one is only a
                # lifeguard, the farther one is squawking 7700. Severity must
                # win over proximity.
                inside = [dict(a, emergency="lifeguard") if a["t"] == "B738"
                          else (dict(a, squawk="7700") if a["t"] == "C172" else a)
                          for a in inside]
            if m == "mil":
                # The military aircraft becomes the nearest, so /v2/closest
                # exercises the dbFlags classification path.
                inside = [a for a in FLEET if a.get("dbFlags", 0) & 1]
            if m == "far":
                # Hide everything inside the primary radius so only the widened
                # fallback search can find an aircraft.
                inside = [a for a in inside if a["_km"] > 20]
            if m == "truncated" and inside:
                body = json.dumps({"ac": [shape(inside[0], lat, lon)], "total": 1})
                return self._send(200, body[:len(body) // 2], clen=len(body), note="truncated")
            sel = inside[:1] if kind == "closest" else inside
            note = f"r={radius_nm}nm ({radius_km:.1f}km) -> {len(sel)} ac"
            return self._send(200, json.dumps(
                {"ac": [shape(a, lat, lon) for a in sel], "total": len(sel),
                 "now": time.time() * 1000, "ctime": time.time() * 1000}), note=note)

        mm = re.fullmatch(r"/v2/(hex|reg|callsign|type|sqk)/(.+)", p)
        if mm:
            key, val = mm.group(1), mm.group(2).upper()
            field = {"hex": "hex", "reg": "r", "callsign": "flight", "type": "t", "sqk": "squawk"}[key]
            hits = [a for a in FLEET if str(a.get(field, "")).strip().upper() == val]
            return self._send(200, json.dumps(
                {"ac": [shape(a, HOME_LAT, HOME_LON) for a in hits], "total": len(hits)}),
                note=f"{key}={val} -> {len(hits)}")

        if p == "/v2/mil":
            n = 4000 if m == "bigmil" else None
            mil = [a for a in FLEET if a.get("dbFlags", 0) & 1]
            if m == "emergency":
                # The 8 km C172 squawks 7700 while the 3 km B738 is normal, so a
                # correct implementation shows the EMERGENCY one, not the nearest.
                inside = [dict(a, squawk="7700") if a["t"] == "C172" else a for a in inside]
            if m == "lifeguard":
                # No squawk equivalent exists: lifeguard lives only in the
                # ADS-B emergency/priority status field.
                inside = [dict(a, emergency="lifeguard") if a["t"] == "C172" else a
                          for a in inside]
            if m == "priority":
                # Two alerting aircraft at once: the NEARER one is only a
                # lifeguard, the farther one is squawking 7700. Severity must
                # win over proximity.
                inside = [dict(a, emergency="lifeguard") if a["t"] == "B738"
                          else (dict(a, squawk="7700") if a["t"] == "C172" else a)
                          for a in inside]
            if m == "mil":
                mil = mil or [FLEET[-1]]
            if n:
                mil = [dict(FLEET[-1], hex=f"{i:06x}") for i in range(n)]
            return self._send(200, json.dumps(
                {"ac": [shape(a, HOME_LAT, HOME_LON) for a in mil], "total": len(mil)}),
                note=f"{len(mil)} mil")

        if p in ("/v2/ladd", "/v2/pia"):
            return self._send(200, json.dumps({"ac": [], "total": 0}))

        return self._send(404, json.dumps({"detail": "Not Found"}), note="unrouted")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8443)
    a = ap.parse_args()
    ensure_cert()
    os.makedirs(LOGDIR, exist_ok=True)
    if not os.path.exists(MODE_FILE):
        open(MODE_FILE, "w").write("ok")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"mock adsb.lol on https://0.0.0.0:{a.port}")
    print(f"  fleet: " + ", ".join(f"{x['t']}@{x['_km']}km" for x in FLEET))
    print(f"  mode file: {MODE_FILE}   request log: {REQ_LOG}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
