#!/usr/bin/env python3
"""BMC telemetry dashboard server (standard library only).

  BMC --UDP CSV (port 9000)--> this server --SSE/HTTP (port 8080)--> browser(s)

* Sends a HELLO datagram to the BMC every couple of seconds so the BMC learns
  where to stream (same mechanism over SoftAP Wi-Fi or Ethernet).
* Parses CSV telemetry, detects sequence gaps, keeps a rolling history, and
  appends every record to logs/telemetry_<timestamp>.csv.
* Serves ./static and pushes live samples to browsers via Server-Sent Events.

Protocol details: see README.md.
"""
import argparse
import collections
import csv
import json
import math
import queue
import socket
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"
DEFAULT_COLUMNS = ["bmc_id", "seq", "t_us", "temp_c", "power_w",
                   "fan_duty_pct", "fan_en", "fan_rpm", "drops", "flags"]
HELLO = b"HELLO\n"
HELLO_PERIOD_S = 2.0
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"}


def parse_value(text):
    text = text.strip()
    if text == "":
        return None
    try:
        return int(text)
    except ValueError:
        pass
    try:
        value = float(text)
        return value if math.isfinite(value) else None
    except ValueError:
        return text


class TelemetryState:
    def __init__(self, history_len, log_dir):
        self.lock = threading.Lock()
        self.columns = list(DEFAULT_COLUMNS)
        self.history = collections.deque(maxlen=history_len)
        self.rx_times = collections.deque(maxlen=200)
        self.last_seq = {}
        self.boot = {}
        self.received = 0
        self.lost = 0
        self.restarts = 0
        self.malformed = 0
        self.last_rx = None            # time.monotonic() of last good record
        self.last_bmc_drops = None
        self.bmc_ids = []
        self.subscribers = []
        self.log_dir = log_dir
        self.log_file = None
        self.log_writer = None
        self.log_cols = None
        self.log_path = None

    # -- logging --------------------------------------------------------
    def _log(self, rec):
        if self.log_dir is None:
            return
        cols = tuple(self.columns)
        if self.log_cols != cols:
            if self.log_file:
                self.log_file.close()
            self.log_dir.mkdir(parents=True, exist_ok=True)
            self.log_path = self.log_dir / f"telemetry_{datetime.now():%Y%m%d_%H%M%S}.csv"
            self.log_file = open(self.log_path, "w", newline="", encoding="utf-8")
            self.log_writer = csv.writer(self.log_file)
            self.log_writer.writerow(["host_time"] + list(cols))
            self.log_cols = cols
        self.log_writer.writerow([f"{rec['rx']:.3f}"] + ["" if rec.get(c) is None else rec.get(c) for c in cols])
        self.log_file.flush()

    # -- ingest ---------------------------------------------------------
    def ingest_datagram(self, data):
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            with self.lock:
                self.malformed += 1
            return
        for line in text.splitlines():
            line = line.strip()
            if line:
                self._ingest_line(line)

    def _ingest_line(self, line):
        fields = line.split(",")
        with self.lock:
            if fields[0].strip() == "bmc_id":        # self-describing header line
                self.columns = [f.strip() for f in fields]
                return
            if len(fields) != len(self.columns):
                self.malformed += 1
                return
            rec = {}
            for name, text in zip(self.columns, fields):
                rec[name] = text.strip() if name == "bmc_id" else parse_value(text)
            if not isinstance(rec.get("seq"), int) or not isinstance(rec.get("t_us"), int):
                self.malformed += 1
                return

            bmc = rec["bmc_id"]
            if bmc not in self.bmc_ids:
                self.bmc_ids.append(bmc)
            last = self.last_seq.get(bmc)
            if last is not None:
                if rec["seq"] == last:
                    return                            # duplicate datagram
                if rec["seq"] > last:
                    self.lost += rec["seq"] - last - 1
                else:                                 # sequence went backwards: BMC rebooted
                    self.restarts += 1
                    self.boot[bmc] = self.boot.get(bmc, 0) + 1
            self.last_seq[bmc] = rec["seq"]
            rec["boot"] = self.boot.get(bmc, 0)
            rec["rx"] = time.time()
            self.received += 1
            self.last_rx = time.monotonic()
            self.rx_times.append(self.last_rx)
            if isinstance(rec.get("drops"), int):
                self.last_bmc_drops = rec["drops"]
            self.history.append(rec)
            self._log(rec)
            payload = json.dumps(rec)
            for q in list(self.subscribers):
                try:
                    q.put_nowait(payload)
                except queue.Full:
                    self.subscribers.remove(q)        # slow client; it will reconnect

    # -- views ----------------------------------------------------------
    def status(self, bmc_addr):
        with self.lock:
            now = time.monotonic()
            recent = [t for t in self.rx_times if now - t <= 5.0]
            rate = (len(recent) - 1) / (recent[-1] - recent[0]) if len(recent) > 1 and recent[-1] > recent[0] else 0.0
            return {
                "received": self.received, "lost": self.lost, "restarts": self.restarts,
                "malformed": self.malformed, "bmc_drops": self.last_bmc_drops,
                "age_s": None if self.last_rx is None else round(now - self.last_rx, 2),
                "rate_hz": round(rate, 1), "bmc_ids": list(self.bmc_ids),
                "bmc_addr": bmc_addr, "log_file": self.log_path.name if self.log_path else None,
            }

    def snapshot(self, n):
        with self.lock:
            samples = list(self.history)[-n:]
        return {"columns": list(self.columns), "samples": samples}

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self.lock:
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)


def udp_loop(sock, state):
    while True:
        try:
            data, _ = sock.recvfrom(4096)
        except OSError:
            continue
        state.ingest_datagram(data)


def hello_loop(sock, bmc_addr):
    while True:
        try:
            sock.sendto(HELLO, bmc_addr)
        except OSError:
            pass                                      # BMC network not reachable yet
        time.sleep(HELLO_PERIOD_S)


def make_handler(state, bmc_addr):
    bmc_text = f"{bmc_addr[0]}:{bmc_addr[1]}"

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urlparse(self.path)
            if url.path == "/events":
                return self._events()
            if url.path == "/api/history":
                n = int(parse_qs(url.query).get("n", ["6000"])[0])
                return self._send(200, json.dumps(state.snapshot(n)).encode(), "application/json")
            if url.path == "/api/status":
                return self._send(200, json.dumps(state.status(bmc_text)).encode(), "application/json")
            name = "index.html" if url.path == "/" else url.path.lstrip("/")
            path = (STATIC / name).resolve()
            if STATIC not in path.parents or not path.is_file():
                return self._send(404, b"not found", "text/plain")
            self._send(200, path.read_bytes(), CONTENT_TYPES.get(path.suffix, "application/octet-stream"))

        def _events(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            q = state.subscribe()
            next_status = 0.0
            try:
                while True:
                    try:
                        payload = q.get(timeout=0.25)
                        self.wfile.write(f"event: sample\ndata: {payload}\n\n".encode())
                    except queue.Empty:
                        pass
                    if time.monotonic() >= next_status:
                        next_status = time.monotonic() + 1.0
                        self.wfile.write(f"event: status\ndata: {json.dumps(state.status(bmc_text))}\n\n".encode())
                    self.wfile.flush()
            except (OSError, ValueError):
                pass
            finally:
                state.unsubscribe(q)

    return Handler


def local_addresses():
    try:
        return sorted({a for a in socket.gethostbyname_ex(socket.gethostname())[2] if not a.startswith("127.")})
    except OSError:
        return []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bmc-ip", default="192.168.4.1", help="BMC address (SoftAP default; Ethernet e.g. 192.168.10.20)")
    ap.add_argument("--bmc-port", type=int, default=9000, help="BMC telemetry/hello port")
    ap.add_argument("--udp-port", type=int, default=9000, help="local UDP port the BMC streams to")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--http-host", default="0.0.0.0", help="use 127.0.0.1 to keep the dashboard local")
    ap.add_argument("--history", type=int, default=6000, help="samples kept in memory (6000 = 10 min at 10 Hz)")
    ap.add_argument("--log-dir", default=str(ROOT / "logs"))
    ap.add_argument("--no-log", action="store_true")
    args = ap.parse_args()

    state = TelemetryState(args.history, None if args.no_log else Path(args.log_dir))
    bmc_addr = (args.bmc_ip, args.bmc_port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind(("0.0.0.0", args.udp_port))
    threading.Thread(target=udp_loop, args=(sock, state), daemon=True).start()
    threading.Thread(target=hello_loop, args=(sock, bmc_addr), daemon=True).start()

    try:
        httpd = ThreadingHTTPServer((args.http_host, args.http_port), make_handler(state, bmc_addr))
    except OSError as exc:
        raise SystemExit(f"Cannot listen on HTTP port {args.http_port} ({exc}).\n"
                         f"Another program is probably using it. Try: python server.py --http-port {args.http_port + 10}")
    httpd.daemon_threads = True
    print(f"UDP telemetry  : listening on :{args.udp_port}, HELLO -> {args.bmc_ip}:{args.bmc_port}")
    print(f"Dashboard      : http://localhost:{args.http_port}")
    if args.http_host == "0.0.0.0":
        for addr in local_addresses():
            print(f"                 http://{addr}:{args.http_port}   (other devices on this network)")
    print(f"CSV log        : {'disabled' if args.no_log else args.log_dir}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")


if __name__ == "__main__":
    main()
