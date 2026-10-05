"""Capture the 10 Hz fan sweep CSV stream from the BMC serial console.

Usage: python tools/sweep_to_csv.py COM5 [out.csv]

Opening the port normally resets the ESP32, which re-runs the boot-time
sweep (FAN_SWEEP_TEST=1). Takes ~1 minute (~550 rows at 10 Hz).
Needs pyserial (ships with ESP-IDF).
"""
import re
import sys
import time
import serial

port = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else time.strftime("fan_sweep_%Y%m%d_%H%M%S.csv")

HEADER = "t_ms,commanded_duty_pct,measured_rpm,steady"
ROW_RE = re.compile(r"^\d+,\d+,\d+,[01]$")

rows, capturing, dropped = [], False, 0
with serial.Serial(port, 115200, timeout=1) as ser:
    print(f"Listening on {port} for sweep output...")
    while True:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        if line == "CSV_BEGIN":
            capturing, rows = True, []
            print("Sweep started...")
        elif line == "CSV_END" and capturing:
            break
        elif capturing:
            # Keep only the header and well-formed data rows; log lines
            # (I/W/E ...) interleaved on the same UART are dropped.
            if line == HEADER:
                rows.append(line)
            elif ROW_RE.match(line):
                rows.append(line)
                if len(rows) % 50 == 0:
                    print(f"  {len(rows) - 1} samples")
            else:
                dropped += 1

with open(out, "w", newline="") as f:
    f.write("\n".join(rows) + "\n")
print(f"Wrote {len(rows) - 1} rows to {out} ({dropped} non-CSV lines ignored)")
