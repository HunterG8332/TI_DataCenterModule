#!/usr/bin/env python3
"""Fake BMC for testing the dashboard without hardware.

Behaves like the planned firmware: waits for HELLO datagrams, then streams CSV
telemetry (with a self-describing header every 200 records) to whoever said hello.

Local test (two terminals):
  python server.py --bmc-ip 127.0.0.1 --bmc-port 9100
  python simulate_bmc.py --port 9100
"""
import argparse
import math
import random
import select
import socket
import time

COLUMNS = "bmc_id,seq,t_us,temp_c,power_w,fan_duty_pct,fan_en,fan_rpm,heater_pct,drops,flags"
SUBSCRIBER_TIMEOUT_S = 10.0
HEATER_TIMEOUT_S = 1.0
F_REQUESTED, F_PERMIT, F_ACTIVE, F_OVER_TEMP, F_TRIPPED = 1, 2, 4, 16, 128


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
    subscribers = {}                       # addr -> last hello time
    seq = 0
    drops = 0
    start = time.monotonic()
    period = 1.0 / args.rate
    next_tick = start
    heater_on, heater_pct, heater_rx, tripped = False, 0.0, None, False
    print(f"fake BMC '{args.bmc_id}' listening on UDP :{args.port}, {args.rate} Hz")

    while True:
        ready, _, _ = select.select([sock], [], [], max(0.0, next_tick - time.monotonic()))
        if ready:
            data, addr = sock.recvfrom(1024)
            if data.startswith(b"HEATER"):
                try:
                    _, on, pct = data.decode().split()
                    heater_on, heater_pct, heater_rx = on == "1", min(100.0, max(0.0, float(pct))), time.monotonic()
                except ValueError:
                    pass
            elif data.startswith(b"HELLO"):
                if addr not in subscribers:
                    print(f"subscriber added: {addr}")
                subscribers[addr] = time.monotonic()
        now = time.monotonic()
        if now < next_tick:
            continue
        next_tick += period

        t = now - start
        temp = 42 + 14 * math.sin(t / 25) + random.gauss(0, 0.05)
        power = 9 + 4 * math.sin(t / 12 + 1) + random.gauss(0, 0.03)
        duty = min(100.0, max(0.0, (temp - 25.0) * 100.0 / 75.0))
        rpm = int(duty * 38 + random.gauss(0, 15)) if duty > 15 else 0
        requested = heater_on and heater_rx is not None and now - heater_rx <= HEATER_TIMEOUT_S
        if not requested:
            tripped = False
        elif temp >= 100.0:
            tripped = True
        permit = not args.no_permit
        active = requested and permit and not tripped
        applied = heater_pct if active else 0.0
        flags = ((F_REQUESTED if requested else 0) | (F_PERMIT if permit else 0) | (F_ACTIVE if active else 0)
                 | (F_TRIPPED if tripped else 0))

        subscribers = {a: ts for a, ts in subscribers.items() if now - ts < SUBSCRIBER_TIMEOUT_S}
        seq += 1
        if random.random() < args.loss:
            drops += 1
            continue                       # seq still advances: host sees a gap
        line = (f"{args.bmc_id},{seq},{int(t * 1e6)},{temp:.4f},{power:.3f},"
                f"{duty:.1f},1,{rpm},{applied:.1f},{drops},{flags}")
        for addr in subscribers:
            if seq % 200 == 1:
                sock.sendto((COLUMNS + "\n").encode(), addr)
            sock.sendto((line + "\n").encode(), addr)


if __name__ == "__main__":
    main()
