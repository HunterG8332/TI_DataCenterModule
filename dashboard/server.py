#!/usr/bin/env python3
"""BMC telemetry dashboard server (standard library only).

  BMC --UDP CSV (port 9000)--> this server --SSE/HTTP (port 8090)--> browser(s)
  browser --POST /api/cmd--> this server --UDP "#n CMD"--> BMC --UDP "#n OK/ERR ..."--> back

* Sends a HELLO datagram to the BMC every couple of seconds so the BMC learns
  where to stream (same mechanism over SoftAP Wi-Fi or Ethernet).
* Relays operator commands (mode, self-test, profile, fault acknowledge ...) to the
  BMC with a token, waits for the matching reply, and retries once. Commands are
  idempotent on the BMC, so a retry is harmless.
* Relays the browser's heater switch/slider to the BMC as "HEATER <0|1> <pct>"
  datagrams every 250 ms. The browser must keep re-posting while the switch is
  on; if it goes quiet (tab closed) the request lapses and the heater turns off.
* Parses CSV telemetry, detects sequence gaps, checks continuity (monotonic
  timestamps, missing required fields), keeps a rolling history, and appends
  every record to logs/telemetry_<timestamp>.csv.
* Captures EVENT lines (failsafe entry with cause and timestamp, self-test report,
  run start/end ...), de-duplicates the BMC's repeated sends, and appends them to
  logs/events_<timestamp>.csv.
* Serves ./static and pushes live samples and events to browsers via Server-Sent Events.

Protocol details: see README.md and docs/bmc_plan/03_interfaces.md.
"""
import argparse
import collections
import csv
import json
import math
import queue
import re
import socket
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"
DEFAULT_COLUMNS = [
    "bmc_id", "seq", "t_us", "run_ms", "mode", "plate_c", "inlet_c", "v48_v", "i48_a", "power_w",
    "v33_v", "v5_v", "v12_v", "heater_cmd_pct", "heater_pct", "fan_demand_pct", "fan_target_rpm",
    "fan1_duty_pct", "fan2_duty_pct", "fan1_rpm", "fan2_rpm", "fan_en", "setpoint_c", "ctl_p",
    "ctl_i", "ctl_ff", "ff_variant", "fault_code", "fault_mask", "flags", "drops"]
# Fields that must be present on every record for the log to count as complete (FSR 3.2.1.6).
# The VRM rail voltages are left out until the VRM monitor exists.
REQUIRED_FIELDS = ["mode", "plate_c", "v48_v", "i48_a", "heater_pct", "fan1_duty_pct", "fan2_duty_pct",
                   "fan1_rpm", "fan2_rpm"]
HELLO = b"HELLO\n"
HELLO_PERIOD_S = 2.0
HEATER_SEND_PERIOD_S = 0.25
HEATER_LEASE_S = 3.0       # browser heartbeat is 1 s; a missed beat or two is tolerated
CMD_TIMEOUT_S = 0.7
CMD_VERBS = {"MODE", "SELFTEST", "ACK", "STATUS", "EVENTS", "FAN", "SETPOINT", "GAINS", "FF",
             "FFPARAM", "PROFILE", "TESTSET", "TESTINJECT"}
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


class CommandRelay:
    """Sends tokenised commands to the BMC and matches the replies."""

    def __init__(self, sock, bmc_addr):
        self.sock = sock
        self.addr = bmc_addr
        self.lock = threading.Lock()
        self.next_token = 1
        self.waiting = {}                    # token -> {"evt": Event, "reply": str | None}
        self.heater_reply = None             # (text, monotonic time) of the latest tokenless HEATER reply

    def send(self, text, timeout=CMD_TIMEOUT_S, retries=1):
        with self.lock:
            token = self.next_token
            self.next_token += 1
            slot = {"evt": threading.Event(), "reply": None}
            self.waiting[token] = slot
        data = f"#{token} {text}\n".encode()
        try:
            for _ in range(retries + 1):
                try:
                    self.sock.sendto(data, self.addr)
                except OSError:
                    return None
                if slot["evt"].wait(timeout):
                    break
        finally:
            with self.lock:
                self.waiting.pop(token, None)
        return slot["reply"]

    def handle_reply(self, line):
        m = re.match(r"#(\d+)\s+(.*)$", line)
        if m:
            with self.lock:
                slot = self.waiting.get(int(m.group(1)))
            if slot is not None:
                slot["reply"] = m.group(2)
                slot["evt"].set()
            return
        if line.startswith("ERR HEATER") or line.startswith("OK HEATER"):
            self.heater_reply = (line, time.monotonic())

    def heater_note(self):
        if self.heater_reply and time.monotonic() - self.heater_reply[1] < 2.0 and self.heater_reply[0].startswith("ERR"):
            return self.heater_reply[0]
        return None


class TelemetryState:
    def __init__(self, history_len, log_dir):
        self.lock = threading.Lock()
        self.columns = list(DEFAULT_COLUMNS)
        self.history = collections.deque(maxlen=history_len)
        self.events = collections.deque(maxlen=500)
        self.seen_events = set()
        self.rx_times = collections.deque(maxlen=200)
        self.last_seq = {}
        self.first_seq = {}
        self.boot = {}
        self.cont = {}                  # per BMC, reset on restart: continuity bookkeeping
        self.received = 0
        self.lost = 0
        self.restarts = 0
        self.malformed = 0
        self.last_rx = None            # time.monotonic() of last good record
        self.last_bmc_drops = None
        self.bmc_ids = []
        self.subscribers = []
        self.on_new_boot = None        # callback: ask the BMC to replay its event log
        self.log_dir = log_dir
        self.log_file = None
        self.log_writer = None
        self.log_cols = None
        self.log_path = None
        self.ev_file = None
        self.ev_writer = None
        self.ev_path = None

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

    def _log_event(self, ev):
        if self.log_dir is None:
            return
        if self.ev_file is None:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            self.ev_path = self.log_dir / f"events_{datetime.now():%Y%m%d_%H%M%S}.csv"
            self.ev_file = open(self.ev_path, "w", newline="", encoding="utf-8")
            self.ev_writer = csv.writer(self.ev_file)
            self.ev_writer.writerow(["host_time", "boot", "id", "t_us", "run_ms", "type", "code", "detail"])
        self.ev_writer.writerow([f"{ev['rx']:.3f}", ev["boot"], ev["id"], ev["t_us"],
                                 "" if ev["run_ms"] is None else ev["run_ms"], ev["type"], ev["code"], ev["detail"]])
        self.ev_file.flush()

    def _push(self, name, obj):
        payload = (name, json.dumps(obj))
        for q in list(self.subscribers):
            try:
                q.put_nowait(payload)
            except queue.Full:
                self.subscribers.remove(q)        # slow client; it will reconnect

    # -- ingest ---------------------------------------------------------
    def ingest_datagram(self, data, relay=None):
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            with self.lock:
                self.malformed += 1
            return
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith(("#", "OK ", "ERR ")):
                if relay is not None:
                    relay.handle_reply(line)
            elif line.startswith("EVENT,"):
                self._ingest_event(line)
            else:
                self._ingest_line(line)

    def _ingest_event(self, line):
        f = line.split(",", 6)                           # EVENT,id,t_us,run_ms,type,code,detail
        try:
            if len(f) != 7:
                raise ValueError
            ev = {"id": int(f[1]), "t_us": int(f[2]), "run_ms": int(f[3]) if f[3] else None,
                  "type": f[4], "code": f[5], "detail": f[6], "rx": time.time()}
        except ValueError:
            with self.lock:
                self.malformed += 1
            return
        with self.lock:
            ev["boot"] = self.restarts
            key = (ev["boot"], ev["id"])
            if key in self.seen_events:
                return                                    # the BMC sends each event three times
            self.seen_events.add(key)
            self.events.append(ev)
            self._log_event(ev)
            self._push("bmcevent", ev)

    def _ingest_line(self, line):
        fields = line.split(",")
        ask_events = False
        with self.lock:
            if fields[0].strip() == "bmc_id":        # self-describing header line
                self.columns = [f.strip() for f in fields]
                return
            if len(fields) != len(self.columns):
                self.malformed += 1
                return
            rec = {}
            for name, text in zip(self.columns, fields):
                rec[name] = text.strip() if name in ("bmc_id", "mode") else parse_value(text)
            if not isinstance(rec.get("seq"), int) or not isinstance(rec.get("t_us"), int):
                self.malformed += 1
                return

            bmc = rec["bmc_id"]
            new_bmc = bmc not in self.bmc_ids
            if new_bmc:
                self.bmc_ids.append(bmc)
            last = self.last_seq.get(bmc)
            restarted = False
            if last is not None:
                if rec["seq"] == last:
                    return                            # duplicate datagram
                if rec["seq"] > last:
                    self.lost += rec["seq"] - last - 1
                else:                                 # sequence went backwards: BMC rebooted
                    self.restarts += 1
                    self.boot[bmc] = self.boot.get(bmc, 0) + 1
                    restarted = True
            if last is None or restarted:
                self.first_seq[bmc] = rec["seq"]
                self.cont[bmc] = {"records": 0, "nonmonotonic": 0, "max_dt_us": 0, "last_t_us": None,
                                  "missing_required": 0}
                ask_events = True
            self.last_seq[bmc] = rec["seq"]

            c = self.cont[bmc]
            c["records"] += 1
            if c["last_t_us"] is not None:
                dt = rec["t_us"] - c["last_t_us"]
                if dt <= 0:
                    c["nonmonotonic"] += 1
                elif dt > c["max_dt_us"]:
                    c["max_dt_us"] = dt
            c["last_t_us"] = rec["t_us"]
            if any(f in rec and rec[f] is None for f in REQUIRED_FIELDS):
                c["missing_required"] += 1

            rec["boot"] = self.boot.get(bmc, 0)
            rec["rx"] = time.time()
            self.received += 1
            self.last_rx = time.monotonic()
            self.rx_times.append(self.last_rx)
            if isinstance(rec.get("drops"), int):
                self.last_bmc_drops = rec["drops"]
            self.history.append(rec)
            self._log(rec)
            self._push("sample", rec)
        if ask_events and self.on_new_boot:
            self.on_new_boot()

    # -- views ----------------------------------------------------------
    def status(self, bmc_addr):
        with self.lock:
            now = time.monotonic()
            recent = [t for t in self.rx_times if now - t <= 5.0]
            rate = (len(recent) - 1) / (recent[-1] - recent[0]) if len(recent) > 1 and recent[-1] > recent[0] else 0.0
            cont = None
            if self.bmc_ids:
                b = self.bmc_ids[0]
                c = self.cont.get(b)
                if c:
                    expected = self.last_seq[b] - self.first_seq[b] + 1
                    cont = {"expected": expected, "received": c["records"],
                            "missing": expected - c["records"], "nonmonotonic": c["nonmonotonic"],
                            "max_gap_s": round(c["max_dt_us"] / 1e6, 3),
                            "missing_required": c["missing_required"]}
            return {
                "received": self.received, "lost": self.lost, "restarts": self.restarts,
                "malformed": self.malformed, "bmc_drops": self.last_bmc_drops,
                "age_s": None if self.last_rx is None else round(now - self.last_rx, 2),
                "rate_hz": round(rate, 1), "bmc_ids": list(self.bmc_ids),
                "bmc_addr": bmc_addr, "log_file": self.log_path.name if self.log_path else None,
                "event_log": self.ev_path.name if self.ev_path else None, "continuity": cont,
            }

    def snapshot(self, n):
        with self.lock:
            samples = list(self.history)[-n:]
            events = list(self.events)
        return {"columns": list(self.columns), "samples": samples, "events": events}

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self.lock:
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)


class HeaterRequest:
    """Latest browser request, valid only for HEATER_LEASE_S after it was posted."""

    def __init__(self):
        self.lock = threading.Lock()
        self.on = False
        self.pct = 0.0
        self.expires = 0.0

    def set(self, on, pct):
        with self.lock:
            self.on = bool(on)
            self.pct = min(100.0, max(0.0, pct))
            self.expires = time.monotonic() + HEATER_LEASE_S

    def effective(self):
        with self.lock:
            return (self.on and time.monotonic() < self.expires), self.pct


def udp_loop(sock, state, relay):
    while True:
        try:
            data, _ = sock.recvfrom(4096)
        except OSError:
            continue
        state.ingest_datagram(data, relay)


def hello_loop(sock, bmc_addr):
    while True:
        try:
            sock.sendto(HELLO, bmc_addr)
        except OSError:
            pass                                      # BMC network not reachable yet
        time.sleep(HELLO_PERIOD_S)


def heater_loop(sock, bmc_addr, heater):
    while True:
        on, pct = heater.effective()
        try:
            sock.sendto(f"HEATER {int(on)} {pct:.1f}\n".encode(), bmc_addr)
        except OSError:
            pass
        time.sleep(HEATER_SEND_PERIOD_S)


def valid_command(text):
    if not isinstance(text, str) or not text.strip() or len(text) > 60:
        return False
    if not all(32 <= ord(ch) < 127 for ch in text):
        return False
    return text.split()[0].upper() in CMD_VERBS


def make_handler(state, bmc_addr, heater, relay, allow_control):
    bmc_text = f"{bmc_addr[0]}:{bmc_addr[1]}"

    def current_status():
        status = state.status(bmc_text)
        on, pct = heater.effective()
        status["heater_cmd"] = {"on": on, "pct": pct}
        status["heater_note"] = relay.heater_note()
        status["control_enabled"] = allow_control
        return status

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

        def _json(self, code, obj):
            self._send(code, json.dumps(obj).encode(), "application/json")

        def do_GET(self):
            url = urlparse(self.path)
            if url.path == "/events":
                return self._events()
            if url.path == "/api/history":
                try:
                    n = int(parse_qs(url.query).get("n", ["6000"])[0])
                except ValueError:
                    return self._send(400, b"bad request", "text/plain")
                return self._json(200, state.snapshot(n))
            if url.path == "/api/status":
                return self._json(200, current_status())
            name = "index.html" if url.path == "/" else url.path.lstrip("/")
            path = (STATIC / name).resolve()
            if STATIC not in path.parents or not path.is_file():
                return self._send(404, b"not found", "text/plain")
            self._send(200, path.read_bytes(), CONTENT_TYPES.get(path.suffix, "application/octet-stream"))

        def _body(self):
            length = min(int(self.headers.get("Content-Length", "0")), 1024)
            return json.loads(self.rfile.read(length) or b"{}")

        def do_POST(self):
            path = urlparse(self.path).path
            if path not in ("/api/heater", "/api/cmd"):
                return self._send(404, b"not found", "text/plain")
            if not allow_control:
                return self._json(403, {"ok": False, "error": "control disabled (server started with --no-control)"})
            try:
                body = self._body()
            except (ValueError, TypeError):
                return self._send(400, b"bad request", "text/plain")
            if path == "/api/cmd":
                text = body.get("cmd") if isinstance(body, dict) else None
                if not valid_command(text):
                    return self._json(400, {"ok": False, "error": "command not allowed"})
                reply = relay.send(text.strip())
                if reply is None:
                    return self._json(200, {"ok": False, "error": "no reply from the BMC (link down?)"})
                return self._json(200, {"ok": reply.startswith("OK"), "reply": reply})
            try:
                on, pct = bool(body.get("on")), float(body.get("pct", 0))
                if not math.isfinite(pct):
                    raise ValueError("pct")
            except (ValueError, TypeError, AttributeError):
                return self._send(400, b"bad request", "text/plain")
            heater.set(on, pct)
            on, pct = heater.effective()
            self._json(200, {"on": on, "pct": pct})

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
                        name, payload = q.get(timeout=0.25)
                        self.wfile.write(f"event: {name}\ndata: {payload}\n\n".encode())
                    except queue.Empty:
                        pass
                    if time.monotonic() >= next_status:
                        next_status = time.monotonic() + 1.0
                        self.wfile.write(f"event: status\ndata: {json.dumps(current_status())}\n\n".encode())
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
    ap.add_argument("--http-port", type=int, default=8090)
    ap.add_argument("--http-host", default="0.0.0.0", help="use 127.0.0.1 to keep the dashboard local")
    ap.add_argument("--history", type=int, default=6000, help="samples kept in memory (6000 = 10 min at 10 Hz)")
    ap.add_argument("--log-dir", default=str(ROOT / "logs"))
    ap.add_argument("--no-log", action="store_true")
    ap.add_argument("--no-control", action="store_true",
                    help="view only: refuse heater and command requests from browsers (use on a shared network)")
    args = ap.parse_args()

    state = TelemetryState(args.history, None if args.no_log else Path(args.log_dir))
    bmc_addr = (args.bmc_ip, args.bmc_port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind(("0.0.0.0", args.udp_port))
    relay = CommandRelay(sock, bmc_addr)
    state.on_new_boot = lambda: threading.Thread(target=relay.send, args=("EVENTS",), daemon=True).start()
    threading.Thread(target=udp_loop, args=(sock, state, relay), daemon=True).start()
    threading.Thread(target=hello_loop, args=(sock, bmc_addr), daemon=True).start()
    heater = HeaterRequest()
    threading.Thread(target=heater_loop, args=(sock, bmc_addr, heater), daemon=True).start()

    try:
        httpd = ThreadingHTTPServer((args.http_host, args.http_port),
                                    make_handler(state, bmc_addr, heater, relay, not args.no_control))
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
    print(f"Control        : {'DISABLED (view only)' if args.no_control else 'enabled (anyone who can open the page can command the BMC)'}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")


if __name__ == "__main__":
    main()
