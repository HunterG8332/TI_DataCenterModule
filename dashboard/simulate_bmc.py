#!/usr/bin/env python3
"""Fake BMC for testing the dashboard without hardware.

It speaks the real wire protocol (telemetry v2 CSV, EVENT lines, tokenised command
replies) and mirrors the firmware's behaviour closely enough to exercise every dashboard
flow: self-test, modes, profiles, failsafe, the ACK rules, fault injection. It is a
Python re-implementation for convenience; the authoritative logic is the C code in
firmware/BMC_firm/components/bmc_core (unit-tested in firmware/BMC_firm/test_host).

Local test (two terminals):
  python server.py --bmc-ip 127.0.0.1 --bmc-port 9100
  python simulate_bmc.py --port 9100

Try: SELFTEST, MODE CHAR, heater switch, PROFILE PRESET 2 / PROFILE START, MODE REACTIVE,
TESTSET OT 40 (lowers the over-temp limit so the plate trips it), then ACK once it cools.
"""
import argparse
import math
import random
import select
import socket
import time

COLUMNS = ("bmc_id,seq,t_us,run_ms,mode,plate_c,inlet_c,v48_v,i48_a,power_w,v33_v,v5_v,v12_v,"
           "heater_cmd_pct,heater_pct,fan_demand_pct,fan_target_rpm,fan1_duty_pct,fan2_duty_pct,"
           "fan1_rpm,fan2_rpm,fan_en,setpoint_c,ctl_p,ctl_i,ctl_ff,ff_variant,fault_code,fault_mask,"
           "flags,drops")
SUBSCRIBER_TIMEOUT_S = 10.0
HEATER_TIMEOUT_S = 1.0
OT_TRIP, OT_WARN, SAFE_CLEAR, SAFE_HOLD_S = 120.0, 110.0, 60.0, 5.0
RPM_REF, FAN_MAX = 7000.0, (7000.0, 10000.0)
FAULTS = ["NONE", "OVER_TEMP", "TEMP_LOSS", "TELEM_LOSS", "I2C_FAULT", "FAN_STALL", "SENSOR_STALE"]
INJECT = {"OT": 1, "OVER_TEMP": 1, "TEMP_LOSS": 2, "TELEM": 3, "I2C": 4, "STALL": 5, "STALE": 6}
F_REQ, F_PERMIT, F_ACTIVE, F_STALL, F_OT, F_TEMPBAD, F_FANOFF, F_TRIP = 1, 2, 4, 8, 16, 32, 64, 128
F_SELFTEST, F_SAFE, F_PROFILE, F_WARN, F_HOOKS, F_LOWERED = 256, 512, 1024, 2048, 4096, 8192
PRESETS = {1: [(60, 0), (600, 100), (300, 0)], 2: [(10, 0), (30, 50), (20, 0)],
           3: [(180, p) for p in (0, 25, 50, 75, 100, 0)]}


class Sim:
    def __init__(self, args):
        self.args = args
        self.t0 = time.monotonic()
        self.now = 0.0
        self.mode = "STANDBY"
        self.selftest_ok = False
        self.selftest_end = 0.0
        self.fault = 0
        self.inject = set()
        self.ot_limit = OT_TRIP
        self.safe = False
        self.cool_since = None
        self.plate = 25.0
        self.ambient = 25.0
        self.permit = not args.no_permit
        self.setpoint, self.kp, self.ki = 60.0, 5.0, 0.2
        self.integ, self.demand, self.ff_variant = 0.0, 0.0, 0
        self.manual_on, self.manual_pct, self.manual_t = False, 0.0, 0.0
        self.fan_auto, self.fan_manual = True, 0.0
        self.steps, self.running, self.run_t0, self.cur = [], False, 0.0, 0
        self.heater_cmd = self.heater = 0.0
        self.events, self.next_id, self.pending = [], 1, []
        self.warned = False
        self.active = set()

    # -- events ---------------------------------------------------------
    def emit(self, typ, code, detail):
        ev = (self.next_id, int(self.now * 1e6), int((self.now - self.run_t0) * 1000) if self.running else None,
              typ, code, detail.replace(",", " "))
        self.next_id += 1
        self.events.append(ev)
        self.events = self.events[-16:]
        self.pending.append([ev, 3, 0.0])

    @staticmethod
    def event_line(ev):
        i, t, run, typ, code, detail = ev
        return f"EVENT,{i},{t},{'' if run is None else run},{typ},{code},{detail}"

    # -- plant ------------------------------------------------------------
    def fan_rpm(self):
        if self.mode == "FAILSAFE":
            return [FAN_MAX[0], FAN_MAX[1]]
        target = min(1.0, max(0.0, self.demand / 100.0)) * RPM_REF
        return [target, target]

    def step(self, dt):
        self.now = time.monotonic() - self.t0
        rpm = self.fan_rpm()
        # control law
        if self.mode in ("REACTIVE", "PROACTIVE"):
            err = self.plate - self.setpoint
            self.integ = min(100.0, max(0.0, self.integ + self.ki * err * dt))
            ff = 10.0 * max(0.0, self.i48()) if self.mode == "PROACTIVE" and self.ff_variant else 0.0
            self.demand = min(100.0, max(0.0, self.kp * err + self.integ + ff))
        elif self.mode == "CHAR":
            self.demand = self.curve() if self.fan_auto else self.fan_manual
        else:
            self.integ = 0.0
            self.demand = self.curve()
        # profile
        cmd = 0.0
        if self.running:
            t = self.now - self.run_t0
            acc = 0.0
            cmd = None
            for dur, pct in self.steps:
                if t < acc + dur:
                    cmd = pct
                    break
                acc += dur
            if cmd is None:
                self.running, cmd = False, 0.0
                self.emit("RUN_END", "complete", f"mode={self.mode}")
        elif self.mode == "CHAR" and self.manual_on and self.now - self.manual_t <= HEATER_TIMEOUT_S:
            cmd = self.manual_pct
        self.heater_cmd = cmd
        allow = self.mode in ("CHAR", "REACTIVE", "PROACTIVE") and self.permit
        self.heater = cmd if allow else 0.0
        # plant
        frac = min(1.0, rpm[0] / RPM_REF)
        g = 0.3 + 1.5 * frac
        self.plate += (100.0 * self.heater / 100.0 - g * (self.plate - self.ambient)) / 200.0 * dt
        # safety
        active = set(self.inject)
        if self.plate >= self.ot_limit:
            active.add(1)
        if self.mode != "FAILSAFE":
            armed = {1} | ({2, 3, 4, 5, 6} if self.mode in ("CHAR", "REACTIVE", "PROACTIVE") else set())
            trip = sorted(active & armed)
            if trip:
                self.fault = trip[0]
                self.mode, self.selftest_ok, self.safe, self.cool_since = "FAILSAFE", False, False, None
                self.running = False
                self.manual_on = False
                self.emit("FAILSAFE_ENTER", FAULTS[self.fault],
                          f"plate_c={self.plate:.1f} limit={self.ot_limit:.1f}")
        else:
            if self.plate < SAFE_CLEAR:
                self.cool_since = self.cool_since if self.cool_since is not None else self.now
                if not self.safe and self.now - self.cool_since >= SAFE_HOLD_S:
                    self.safe = True
                    self.emit("SAFE_TO_HANDLE", "SAFE", f"plate_c={self.plate:.1f}")
            else:
                self.cool_since, self.safe = None, False
        if self.plate >= OT_WARN and not self.warned:
            self.emit("WARN", "OT_WARN", f"plate_c={self.plate:.1f}")
        self.warned = self.plate >= OT_WARN
        if self.mode == "SELFTEST" and self.now >= self.selftest_end:
            self.mode, self.selftest_ok = "STANDBY", True
            self.emit("SELFTEST", "PASS", "PASS enum ambient v48 interlock fan1 fan2 heater_off")
        self.active = active

    def curve(self):
        return min(100.0, max(0.0, (self.plate - 25.0) * 100.0 / 75.0))

    def i48(self):
        return self.heater / 48.0 + 0.2

    # -- telemetry ---------------------------------------------------------------
    def record(self, seq, drops):
        rpm = self.fan_rpm()
        duty = [100.0, 100.0] if self.mode == "FAILSAFE" else [self.demand / 100.0 * RPM_REF / m * 100.0 for m in FAN_MAX]
        run_ms = int((self.now - self.run_t0) * 1000) if self.running else ""
        mask = sum(1 << f for f in self.active)
        flags = ((F_REQ if self.heater_cmd > 0 else 0) | (F_PERMIT if self.permit else 0)
                 | (F_ACTIVE if self.heater > 0 else 0) | (F_OT if 1 in self.active else 0)
                 | (F_TRIP if self.mode == "FAILSAFE" else 0) | (F_SELFTEST if self.selftest_ok else 0)
                 | (F_SAFE if self.safe else 0) | (F_PROFILE if self.running else 0)
                 | (F_WARN if self.plate >= OT_WARN else 0) | F_HOOKS
                 | (F_LOWERED if self.ot_limit < OT_TRIP else 0))
        v48, i48 = 48.0 + random.gauss(0, 0.02), self.i48() + random.gauss(0, 0.005)
        return (f"{self.args.bmc_id},{seq},{int(self.now * 1e6)},{run_ms},{self.mode},"
                f"{self.plate + random.gauss(0, 0.03):.4f},,{v48:.3f},{i48:.4f},{v48 * i48:.3f},,,,"
                f"{self.heater_cmd:.1f},{self.heater:.1f},{self.demand:.1f},{int(self.demand / 100 * RPM_REF)},"
                f"{duty[0]:.1f},{duty[1]:.1f},{int(rpm[0] + random.gauss(0, 8))},{int(rpm[1] + random.gauss(0, 10))},1,"
                f"{self.setpoint:.1f},{self.kp * (self.plate - self.setpoint):.2f},{self.integ:.2f},0.00,"
                f"{self.ff_variant},{self.fault if self.mode == 'FAILSAFE' else 0},{mask},{flags},{drops}")

    # -- commands ------------------------------------------------------------------
    def command(self, line):
        tok = line.strip().split()
        if not tok:
            return ""
        pre = ""
        if tok[0].startswith("#"):
            pre, tok = tok[0] + " ", tok[1:]
            if not tok:
                return ""
        verb, args = tok[0].upper(), [t.upper() for t in tok[1:]]

        def ok(info=""):
            return f"{pre}OK {verb}{(' ' + info) if info else ''}"

        def err(why):
            return f"{pre}ERR {verb} {why}"

        actuating = self.mode in ("CHAR", "REACTIVE", "PROACTIVE")
        if verb == "HELLO":
            return ""
        if verb == "STATUS":
            return ok(f"mode={self.mode} selftest={int(self.selftest_ok)} fault={FAULTS[self.fault] if self.mode == 'FAILSAFE' else 'NONE'} "
                      f"plate_c={self.plate:.1f} setpoint_c={self.setpoint:.1f}")
        if verb == "EVENTS":
            lines = [ok(f"count={len(self.events)}")] + [self.event_line(e) for e in self.events]
            return "\n".join(lines)
        if verb == "SELFTEST":
            if self.mode == "FAILSAFE":
                return err("latched")
            if self.mode != "STANDBY":
                return err("bad_state")
            self.mode, self.selftest_ok, self.selftest_end = "SELFTEST", False, self.now + 6.5
            return ok("started")
        if verb == "MODE" and len(args) == 1 and args[0] in ("STANDBY", "CHAR", "REACTIVE", "PROACTIVE"):
            tgt = args[0]
            if self.mode == "FAILSAFE":
                return err("latched")
            if self.mode == "SELFTEST":
                return err("busy")
            if tgt != "STANDBY" and not self.selftest_ok:
                return err("no_selftest")
            if tgt != "STANDBY" and actuating and tgt != self.mode:
                if self.running:
                    return err("profile_running")
                if self.heater > 0:
                    return err("heater_on")
            if tgt != self.mode:
                self.running = False
                self.manual_on, self.fan_auto, self.integ = False, True, 0.0
                self.emit("MODE_CHANGE", tgt, f"from={self.mode} why=request")
                self.mode = tgt
            return ok(self.mode)
        if verb == "ACK":
            if self.mode != "FAILSAFE":
                return err("bad_state")
            if not self.safe or self.plate >= SAFE_CLEAR:
                return err("not_safe")
            if self.active:
                return err("fault_active")
            self.emit("ACK_OK", FAULTS[self.fault], f"plate_c={self.plate:.1f}")
            self.mode, self.fault, self.safe, self.selftest_ok = "STANDBY", 0, False, False
            self.ot_limit, self.inject = OT_TRIP, set()
            return ok("STANDBY")
        if verb == "HEATER" and len(args) == 2:
            try:
                on, pct = args[0] == "1", min(100.0, max(0.0, float(args[1])))
            except ValueError:
                return err("args")
            if not on:
                self.manual_on = False
                return ""
            if self.mode != "CHAR":
                return err("mode")
            self.manual_on, self.manual_pct, self.manual_t = True, pct, self.now
            return ""
        if verb == "FAN" and len(args) == 1:
            if self.mode != "CHAR":
                return err("mode")
            if args[0] == "AUTO":
                self.fan_auto = True
                return ok("AUTO")
            try:
                self.fan_manual, self.fan_auto = min(100.0, max(0.0, float(args[0]))), False
            except ValueError:
                return err("args")
            return ok(f"{self.fan_manual:.1f}")
        if verb in ("SETPOINT", "GAINS", "FF", "FFPARAM") and (self.running or self.mode in ("FAILSAFE", "SELFTEST")):
            return err("profile_running" if self.running else ("latched" if self.mode == "FAILSAFE" else "busy"))
        if verb == "SETPOINT" and len(args) == 1:
            try:
                v = float(args[0])
            except ValueError:
                return err("args")
            if not 20.0 <= v <= OT_WARN:
                return err("range")
            self.setpoint = v
            return ok(f"{v:.1f}")
        if verb == "GAINS" and len(args) == 3:
            try:
                kp, ki, kd = (float(a) for a in args)
            except ValueError:
                return err("args")
            if min(kp, ki, kd) < 0 or max(kp, ki, kd) > 1000:
                return err("range")
            self.kp, self.ki, self.integ = kp, ki, 0.0
            return ok(f"kp={kp:.3f} ki={ki:.3f} kd={kd:.3f}")
        if verb == "FF" and len(args) == 1 and args[0].isdigit():
            if int(args[0]) > 3:
                return err("range")
            self.ff_variant = int(args[0])
            return ok(str(self.ff_variant))
        if verb == "PROFILE" and args:
            sub = args[0]
            if sub == "STOP":
                if self.running:
                    self.running = False
                    self.emit("RUN_END", "stopped", f"mode={self.mode}")
                self.manual_on = False
                return ok("STOP")
            if self.running:
                return err("profile_running")
            if self.mode in ("FAILSAFE", "SELFTEST") and sub != "START":
                return err("latched" if self.mode == "FAILSAFE" else "busy")
            if sub == "CLEAR":
                self.steps = []
                return ok("CLEAR")
            if sub == "ADD" and len(args) == 3:
                try:
                    d, p = int(args[1]), float(args[2])
                except ValueError:
                    return err("args")
                if d <= 0 or not 0 <= p <= 100 or len(self.steps) >= 16:
                    return err("range")
                self.steps.append((d, p))
                return ok(f"ADD steps={len(self.steps)}")
            if sub == "PRESET" and len(args) == 2 and args[1].isdigit() and int(args[1]) in PRESETS:
                self.steps = list(PRESETS[int(args[1])])
                return ok(f"PRESET {args[1]} steps={len(self.steps)} total_s={sum(d for d, _ in self.steps)}")
            if sub == "START":
                if not actuating:
                    return err("latched" if self.mode == "FAILSAFE" else "mode")
                if not self.steps:
                    return err("no_profile")
                if self.plate > 40.0:
                    return err("not_at_ambient")
                self.running, self.run_t0 = True, self.now
                self.emit("RUN_START", self.mode, f"profile_steps={len(self.steps)} setpoint={self.setpoint:.1f} kp={self.kp:.3f} ki={self.ki:.3f}")
                return ok("START")
        if verb == "TESTSET" and len(args) == 2 and args[0] in ("OT", "OT_LIMIT"):
            try:
                self.ot_limit = min(OT_TRIP, float(args[1]))
            except ValueError:
                return err("args")
            return ok(f"ot_limit_c={self.ot_limit:.1f}")
        if verb == "TESTINJECT" and len(args) == 1:
            if args[0] == "CLEAR":
                self.inject = set()
                return ok("cleared")
            if args[0] in INJECT:
                self.inject.add(INJECT[args[0]])
                return ok(FAULTS[INJECT[args[0]]])
        return err("unknown" if verb not in ("MODE", "HEATER", "FAN", "SETPOINT", "GAINS", "FF", "PROFILE", "TESTSET", "TESTINJECT") else "args")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9100, help="port this fake BMC listens on")
    ap.add_argument("--rate", type=float, default=10.0, help="records per second")
    ap.add_argument("--loss", type=float, default=0.0, help="fraction of records to skip (simulates drops)")
    ap.add_argument("--bmc-id", default="bmc0")
    ap.add_argument("--no-permit", action="store_true", help="simulate the hardware heater permit switch being open")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    sim = Sim(args)
    subscribers = {}                       # addr -> [last hello time, need_header]
    seq = drops = 0
    last_step = time.monotonic()
    next_tick = last_step
    period = 1.0 / args.rate
    print(f"fake BMC '{args.bmc_id}' listening on UDP :{args.port}, {args.rate} Hz (boots in STANDBY, self-test required)")

    while True:
        ready, _, _ = select.select([sock], [], [], 0.02)
        while ready:
            data, addr = sock.recvfrom(1024)
            text = data.decode(errors="replace")
            if text.startswith("HELLO"):
                if addr not in subscribers:
                    print(f"subscriber added: {addr}")
                    subscribers[addr] = [time.monotonic(), True]
                subscribers[addr][0] = time.monotonic()
            elif addr in subscribers:
                reply = sim.command(text.splitlines()[0] if text.splitlines() else "")
                if reply:
                    sock.sendto(reply.encode(), addr)
            ready, _, _ = select.select([sock], [], [], 0)
        now = time.monotonic()
        if now - last_step >= 0.05:
            sim.step(now - last_step)
            last_step = now
        for p in sim.pending:                              # each event is sent three times, 100 ms apart
            if p[1] > 0 and sim.now >= p[2]:
                for addr in subscribers:
                    sock.sendto((sim.event_line(p[0]) + "\n").encode(), addr)
                p[1] -= 1
                p[2] = sim.now + 0.1
        sim.pending = [p for p in sim.pending if p[1] > 0]
        subscribers = {a: v for a, v in subscribers.items() if now - v[0] < SUBSCRIBER_TIMEOUT_S}
        if now >= next_tick:
            next_tick += period
            seq += 1
            if random.random() < args.loss:
                drops += 1
                continue                   # seq still advances: host sees a gap
            line = sim.record(seq, drops)
            for addr, v in subscribers.items():
                if v[1] or seq % 200 == 1:
                    sock.sendto((COLUMNS + "\n").encode(), addr)
                    v[1] = False
                sock.sendto((line + "\n").encode(), addr)


if __name__ == "__main__":
    main()
