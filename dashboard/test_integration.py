#!/usr/bin/env python3
"""End-to-end check of server.py against the simulator (no hardware, stdlib only).

    python test_integration.py

Starts simulate_bmc.py and server.py on spare ports, then drives the dashboard API
the way the browser does: commands and their refusals, failsafe entry, event capture and
de-duplication, the ACK rules, continuity statistics, and the --no-control switch.
Takes about 30 seconds.
"""
import json
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SIM_PORT, UDP_PORT, HTTP_PORT = 19100, 19000, 18090
BASE = f"http://127.0.0.1:{HTTP_PORT}"
failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL: {what}")


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=3) as r:
        return json.loads(r.read())


def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def cmd(text):
    return post("/api/cmd", {"cmd": text})


def last_sample():
    s = get("/api/history?n=1")["samples"]
    return s[-1] if s else None


def wait_for(pred, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pred():
                return True
        except (urllib.error.URLError, ConnectionError, KeyError, IndexError):
            pass
        time.sleep(0.2)
    check(False, f"timed out waiting for {what}")
    return False


def start_server(log_dir, extra=()):
    return subprocess.Popen([sys.executable, "-I", str(HERE / "server.py"), "--bmc-ip", "127.0.0.1",
                             "--bmc-port", str(SIM_PORT), "--udp-port", str(UDP_PORT), "--http-port", str(HTTP_PORT),
                             "--http-host", "127.0.0.1", "--log-dir", log_dir, *extra],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main():
    log_dir = tempfile.mkdtemp(prefix="bmc_it_")
    sim = subprocess.Popen([sys.executable, "-I", str(HERE / "simulate_bmc.py"), "--port", str(SIM_PORT)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    server = start_server(log_dir)
    try:
        print("waiting for telemetry ...")
        if not wait_for(lambda: get("/api/status")["age_s"] is not None and get("/api/status")["age_s"] < 2, 15, "first telemetry"):
            return
        s = last_sample()
        check(s["mode"] == "STANDBY" and not (s["flags"] & 256), "boots in STANDBY with self-test not passed")

        print("commands and refusals ...")
        code, r = cmd("STATUS")
        check(code == 200 and r["ok"] and "mode=STANDBY" in r["reply"], "STATUS reply")
        code, r = cmd("MODE CHAR")
        check(not r["ok"] and "no_selftest" in r["reply"], "MODE CHAR refused before the self-test")
        code, r = cmd("HEATER 1 50")
        check(code == 400, "HEATER is not a dashboard command verb")
        code, r = cmd("REBOOT")
        check(code == 400, "unknown verb rejected before it reaches the BMC")
        code, r = cmd("MODE CHAR; DROP")
        check(not r.get("ok"), "malformed command not accepted")
        code, r = cmd("X" * 80)
        check(code == 400, "over-long command rejected")

        code, r = cmd("SELFTEST")
        check(r["ok"], "SELFTEST accepted")
        wait_for(lambda: last_sample()["mode"] == "SELFTEST", 3, "SELFTEST mode")
        code, r = cmd("MODE STANDBY")
        check(not r["ok"] and "busy" in r["reply"], "mode change refused during the self-test")
        wait_for(lambda: last_sample()["mode"] == "STANDBY" and last_sample()["flags"] & 256, 15, "self-test pass")
        code, r = cmd("MODE CHAR")
        check(r["ok"], "MODE CHAR accepted after the self-test")
        wait_for(lambda: last_sample()["mode"] == "CHAR", 3, "CHAR mode")
        code, r = cmd("SETPOINT 5")
        check(not r["ok"] and "range" in r["reply"], "setpoint range enforced")

        print("failsafe and events ...")
        code, r = cmd("TESTINJECT STALL")
        check(r["ok"], "fault injection accepted (simulator has test hooks)")
        wait_for(lambda: last_sample()["mode"] == "FAILSAFE", 5, "failsafe entry")
        s = last_sample()
        check(s["fault_code"] == 5, f"fault code is FAN_STALL (got {s['fault_code']})")
        check(s["heater_pct"] == 0 and s["flags"] & 128, "heater off and latched flag set")
        time.sleep(0.6)                                    # let all three event repeats arrive
        evs = get("/api/history?n=1")["events"]
        fs = [e for e in evs if e["type"] == "FAILSAFE_ENTER"]
        check(len(fs) == 1, f"exactly one FAILSAFE_ENTER despite 3 sends (got {len(fs)})")
        check(fs and fs[0]["code"] == "FAN_STALL" and fs[0]["t_us"] > 0, "event names the cause and carries a timestamp")

        code, r = cmd("ACK")
        check(not r["ok"], "ACK refused while the fault is still active / not yet safe")
        code, r = cmd("MODE CHAR")
        check(not r["ok"] and "latched" in r["reply"], "actuating modes refused while latched")
        cmd("TESTINJECT CLEAR")
        print("  waiting for the safe-to-handle hold (5 s) ...")
        wait_for(lambda: last_sample()["flags"] & 512, 12, "SAFE_TO_HANDLE")
        code, r = cmd("ACK")
        check(r["ok"], "ACK accepted when safe")
        wait_for(lambda: last_sample()["mode"] == "STANDBY", 3, "return to STANDBY")
        s = last_sample()
        check(not (s["flags"] & 256), "self-test must be run again after a failsafe")
        code, r = cmd("MODE CHAR")
        check(not r["ok"] and "no_selftest" in r["reply"], "heat blocked until the self-test passes again")

        print("continuity and logs ...")
        st = get("/api/status")
        c = st["continuity"]
        check(c is not None and c["nonmonotonic"] == 0, "timestamps are monotonic")
        check(c["missing_required"] == 0, f"no record missing a required field ({c['missing_required']})")
        check(c["max_gap_s"] < 0.5, f"no gap beyond a few sample intervals (max {c['max_gap_s']} s)")
        check(c["missing"] == 0 and st["lost"] == 0, "no lost records on loopback")
        logs = list(Path(log_dir).glob("telemetry_*.csv"))
        evlogs = list(Path(log_dir).glob("events_*.csv"))
        check(len(logs) == 1 and logs[0].stat().st_size > 1000, "telemetry log written")
        check(len(evlogs) == 1 and "FAILSAFE_ENTER" in evlogs[0].read_text(), "event log written with the failsafe")

        print("view-only server ...")
        server.terminate()
        server.wait(5)
        server = start_server(log_dir, ("--no-control", "--no-log"))
        wait_for(lambda: get("/api/status")["control_enabled"] is False, 8, "server restart")
        code, r = cmd("STATUS")
        check(code == 403, "commands refused with --no-control")
        code, r = post("/api/heater", {"on": True, "pct": 10})
        check(code == 403, "heater requests refused with --no-control")
    finally:
        for p in (server, sim):
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
    print(f"\n{checks} checks, {failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
