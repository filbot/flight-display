#!/usr/bin/env python3
"""Local HTTPS mock of api.adsb.lol for fault injection.

The firmware calls client.setInsecure(), so a self-signed cert is accepted;
this generates one on first run (needs openssl, ships with macOS).

    tools/mockapi.py                 # listen on 0.0.0.0:8443
    tools/mockapi.py --port 9443

The fault mode is read from logs/mock.mode on every request, so tools/faults.py
can change behaviour mid-flight without restarting the server. Modes:

    ok          valid response, one aircraft
    empty       valid response, no aircraft ("no aircraft nearby" path)
    http403     403 with adsb.lol's real "too generic" body
    http429     429 rate limited
    http500     500 server error
    malformed   200 with JSON that does not parse
    truncated   Content-Length promises more than is sent (tests the read loop)
    slow        responds after SLOW_SECONDS, exercising read timeouts
    hang        accepts, sends nothing, never closes
    garbage     200 with binary junk
    bigmil      /v2/mil returns a very large body
"""
import argparse, json, os, ssl, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.environ.get("LOGDIR", os.path.join(ROOT, "logs"))
MODE_FILE = os.path.join(LOGDIR, "mock.mode")
CERT = os.path.join(LOGDIR, "mock-cert.pem")
KEY = os.path.join(LOGDIR, "mock-key.pem")
SLOW_SECONDS = 40  # must exceed the firmware's HTTP_READ_TIMEOUT_MS (30s)

AIRCRAFT = {
    "hex": "a1b2c3", "type": "adsb_icao", "flight": "MOCK123 ", "r": "N0MOCK",
    "t": "B738", "alt_baro": 12345, "alt_geom": 12400, "gs": 300.0, "track": 90.0,
    "squawk": "1200", "category": "A3", "lat": 47.6510, "lon": -122.3700,
    "seen_pos": 1.0, "dbFlags": 0, "desc": "BOEING 737-800",
}


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
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", KEY, "-out", CERT, "-days", "3650",
         "-subj", "/CN=mock-adsb"],
        check=True, capture_output=True)
    print(f"generated self-signed cert in {LOGDIR}")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        print(f"{time.strftime('%H:%M:%S')} [{mode()}] {self.path} {fmt % a}")

    def _send(self, code, body, ctype="application/json", clen=None):
        raw = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        # clen overrides the real length on purpose for the truncated case
        self.send_header("Content-Length", str(clen if clen is not None else len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        m = mode()
        if m == "hang":
            time.sleep(600)  # accepted, never answered
            return
        if m == "slow":
            time.sleep(SLOW_SECONDS)
        if self.path.startswith("/v2/mil"):
            n = 4000 if m == "bigmil" else 40
            return self._send(200, json.dumps({"ac": [dict(AIRCRAFT, hex=f"{i:06x}") for i in range(n)]}))
        if m == "http403":
            return self._send(403, "User-Agent too generic; include valid contact info.\n", "text/plain")
        if m == "http429":
            return self._send(429, "Too Many Requests\n", "text/plain")
        if m == "http500":
            return self._send(500, "Internal Server Error\n", "text/plain")
        if m == "malformed":
            return self._send(200, '{"ac":[{"hex":"a1b2c3",,,]}NOT JSON')
        if m == "garbage":
            return self._send(200, bytes(range(256)) * 8, "application/octet-stream")
        if m == "truncated":
            body = json.dumps({"ac": [AIRCRAFT], "total": 1})
            cut = body[:len(body) // 2]  # genuinely severed mid-object
            # Content-Length promises the whole body; the client must notice the
            # stream ended early rather than parsing whatever arrived.
            return self._send(200, cut, clen=len(body))
        if m == "empty":
            return self._send(200, json.dumps({"ac": [], "total": 0}))
        return self._send(200, json.dumps({"ac": [AIRCRAFT], "total": 1, "now": time.time()}))


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
    print(f"mock adsb.lol on https://0.0.0.0:{a.port}  mode file: {MODE_FILE}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
