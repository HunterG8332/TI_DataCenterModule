# BMC Telemetry Dashboard

Live browser dashboard and operator console for the BMC. Python standard library only (3.8+), no installs.

```
 ┌────────────── ESP32 BMC ──────────────┐            ┌──────────── host computer ────────────┐
 │ core 1: supervisor (safety+fans) 50ms │            │ server.py                              │
 │         sensors 10 Hz ─ telemetry_publish()        │   UDP :9000  ─► parse, gap-detect,     │      ┌─────────┐
 │ core 0: net_tx task ── bmc_net ───────┼─ UDP/CSV ─►│                 history, CSV log       ├─SSE─►│ browser │
 │         (SoftAP now, Ethernet later)  │◄ HELLO/cmd ┤   HTTP :8090 ─► static/ /events /api   │      └─────────┘
 └───────────────────────────────────────┘            └────────────────────────────────────────┘
```

## Run it

```powershell
cd dashboard
python server.py                  # real BMC on its SoftAP (BMC = 192.168.4.1)
# open http://localhost:8090
```

No hardware? Run a fake BMC that speaks the same protocol (and has the same modes, self-test, failsafe and ACK rules):

```powershell
python server.py --bmc-ip 127.0.0.1 --bmc-port 9100
python simulate_bmc.py --port 9100 --loss 0.03     # --loss simulates dropped samples
                                                   # --no-permit simulates the open heater permit switch
python test_integration.py                         # automated end-to-end check of server + simulator
```

**View only:** `python server.py --no-control` refuses every command and heater request. Use it on a shared network:
with control enabled, anyone who can open the page can command the BMC (there is no authentication; the lab
rack's real emergency stop is the bench supply). `--http-host 127.0.0.1` keeps the page local.

Windows firewall must allow inbound **UDP 9000** and **TCP 8090** for Python (admin PowerShell):

```powershell
New-NetFirewallRule -DisplayName "BMC dashboard UDP" -Direction Inbound -Protocol UDP -LocalPort 9000 -Action Allow
New-NetFirewallRule -DisplayName "BMC dashboard HTTP" -Direction Inbound -Protocol TCP -LocalPort 8090 -Action Allow
```

Every record is appended to `logs/telemetry_<timestamp>.csv`, every event to `logs/events_<timestamp>.csv`
(both git-ignored). `--no-log` disables them.

## Operating the BMC

The page follows the operating sequence in the CONOPS. The BMC makes every decision; the buttons only send
requests, and a button is enabled only when the BMC's telemetry says the request can be accepted.

1. **Self-test** (needs Standby). About 7 s; fans spin one at a time. Required before any heat.
2. **Mode**: Standby, Characterization (manual heater and fan), Reactive (PID on plate temperature),
   Proactive (feedforward from load current plus trim).
3. **Profile**: pick a preset and *Load + start*. The BMC runs the timeline itself, so a lost Wi-Fi link does not
   change the run. A run only starts when the plate is near ambient. Setpoint, gains and feedforward are locked
   during a run.
4. **Fault**: after a failsafe, *Acknowledge fault* becomes available once the plate has stayed below the
   safe-to-handle temperature (60 °C) for 5 s and no fault condition remains. Acknowledging returns to Standby
   and the self-test must pass again before heat.

The *Manual heater* switch (the old dashboard control) only works in Characterization mode.
The *Validation test hooks* box appears only when the firmware build has them enabled.

## Wire protocol (identical over Wi-Fi and Ethernet)

Full detail: `docs/bmc_plan/03_interfaces.md`.

**Subscribe.** The server sends `HELLO\n` (UDP) to the BMC every 2 s from its telemetry port. The BMC remembers
each sender (up to 4), streams to it, and accepts commands only from it; it forgets a sender after 10 s without
a HELLO. Switching to Ethernet only changes `--bmc-ip`.

**Telemetry.** One CSV record per datagram, UDP port 9000, 10 Hz. A header line precedes the first record, repeats
every 200 records, and is resent to each new subscriber. The server maps fields **by header name**, so adding a
column never breaks the host.

```
bmc_id,seq,t_us,run_ms,mode,plate_c,inlet_c,v48_v,i48_a,power_w,v33_v,v5_v,v12_v,heater_cmd_pct,heater_pct,
fan_demand_pct,fan_target_rpm,fan1_duty_pct,fan2_duty_pct,fan1_rpm,fan2_rpm,fan_en,setpoint_c,ctl_p,ctl_i,
ctl_ff,ff_variant,fault_code,fault_mask,flags,drops
```

| Field | Meaning |
|---|---|
| `seq` | Monotonic per boot. Gaps = datagrams lost in transit; going backwards = BMC reboot |
| `t_us` | Microseconds since boot, stamped at the BMC (the time axis of every chart) |
| `run_ms` | Milliseconds since `PROFILE START`; empty outside a run |
| `mode` | `STANDBY`, `SELFTEST`, `CHAR`, `REACTIVE`, `PROACTIVE`, `FAILSAFE` |
| `plate_c` | Spreader-plate temperature. **Empty = reading unavailable** (never a fake zero) |
| `inlet_c`, `v48_v`, `i48_a`, `power_w`, `v33_v`, `v5_v`, `v12_v` | Inlet air, 48 V bus voltage/current/power, VRM rails (rails and inlet empty until fitted) |
| `heater_cmd_pct`, `heater_pct` | Requested by the profile/manual control, and actually applied after the safety checks |
| `fan_demand_pct`, `fan_target_rpm` | Control-law output as a percentage of the 7 000 RPM reference, and that RPM |
| `fan1_*`, `fan2_*`, `fan_en` | Applied duty and measured RPM per fan; interlock closed (0/1) |
| `setpoint_c`, `ctl_p`, `ctl_i`, `ctl_ff`, `ff_variant` | Control-law internals for analysis |
| `fault_code` | Latched first cause in failsafe: 0 none, 1 OVER_TEMP, 2 TEMP_LOSS, 3 TELEM_LOSS, 4 I2C_FAULT, 5 FAN_STALL, 6 SENSOR_STALE, 7–9 reset reasons |
| `fault_mask` | Bit per fault code, set while that condition is active right now |
| `flags` | bit0 heater requested, 1 permit closed, 2 heater active, 3 fan stall, 4 over-temp, 5 temp invalid, 6 fan interlock open, 7 failsafe latched, 8 self-test passed, 9 safe to handle, 10 profile running, 11 over-temp warning, 12 test hooks compiled in, 13 over-temp limit lowered |
| `drops` | Records the BMC itself discarded (queue full or link down) |

**Events.** `EVENT,<id>,<t_us>,<run_ms>,<type>,<code>,<detail>`: failsafe entry (cause, plate temperature, timestamp),
safe-to-handle, ack, self-test report, mode changes, run start (with every control parameter, so each dataset
describes itself), run end, reset. The BMC sends each event three times; the server de-duplicates on the id and
requests a replay (`EVENTS`) whenever it sees a new boot.

**Commands.** `POST /api/cmd {"cmd": "MODE CHAR"}` is relayed as `#<n> MODE CHAR` (UDP). The BMC answers
`#<n> OK …` or `#<n> ERR <command> <reason>`; the server waits 0.7 s and retries once (commands are idempotent).
Commands: `STATUS`, `EVENTS`, `SELFTEST`, `MODE <STANDBY|CHAR|REACTIVE|PROACTIVE>`, `ACK`, `FAN <pct|AUTO>`,
`SETPOINT`, `GAINS kp ki kd`, `FF <id>`, `FFPARAM id idx value`, `PROFILE CLEAR|ADD s pct|PRESET n|START|STOP`,
and in test-hook builds `TESTSET OT <c>` / `TESTINJECT <fault|CLEAR>`.

**Manual heater.** The switch + slider become `HEATER <0|1> <pct>` datagrams every 250 ms; the browser re-posts
`/api/heater` every second while the switch is on, the server stops asserting ON 3 s after the last post, and the
BMC treats 1 s of silence as off. It is only a *request*: the BMC applies heat only in Characterization mode and
only while the permit pin is closed, the fan interlock is closed and the plate temperature is readable. A trip
into failsafe overrides it at any time.

## Continuity checks (validation: telemetry logging continuity)

The status panel reports, per run: records received vs expected from the sequence numbers, lost, longest gap,
out-of-order timestamps, and records with a required field missing. For the validation plan these must be 0, 0,
about 0.1 s, 0 and 0.

## Firmware map

| Component | Role |
|---|---|
| `bmc_core` | All decisions, no hardware: mode state machine and faults, self-test, PID, RPM matching, profiles, feedforward, command parsing, telemetry/event encoding. Unit-tested on a PC. |
| `bmc_app` | Hardware glue: `sensors` (I2C + failure counting + telemetry), `supervisor` (50 ms safety/fan task), `actuators` (the only writer of heater and fan outputs), `aux` (OLED, fan-link peer). |
| `bmc_stream` | Core-0 net task: telemetry, events (sent 3×), command intake, replies. The safety task never calls the network stack. |
| `bmc_net` | The only file that knows the link (Wi-Fi SoftAP now, W5500 later). |
| `tmp117`, `ina238`, `ssd1306`, `bmc_fan`, `bmc_heater_cmd`, `bmc_i2c`, `bmc_board` | Device drivers and the board pin map. |

Settings: `idf.py menuconfig` → *BMC network*, *BMC telemetry stream*, *BMC application* (test hooks, inlet sensor).
**Password note:** `sdkconfig` is tracked in git, so the configured Wi-Fi password is committed with it. The default
(`bmctelemetry`) is fine for the bench; change it before sharing the repo widely.

## Files

| Path | Purpose |
|---|---|
| `server.py` | UDP receiver/HELLO sender, command relay, event capture, history, CSV/event logs, HTTP/SSE server |
| `simulate_bmc.py` | Fake BMC for testing without hardware |
| `test_integration.py` | End-to-end check of server + simulator (about 30 s, no hardware) |
| `static/` | The page (`index.html`, `app.js`, `style.css`), no external dependencies, works offline |
