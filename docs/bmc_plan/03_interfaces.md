# 03 — Interfaces: Telemetry v2, Events, Commands, GPIO, I2C

Host link remains UDP (D5), BMC → host on port 9000, as today. The dashboard's HELLO mechanism stays: the host announces itself, the BMC streams to every HELLO source, and commands are accepted **only from addresses that are current subscribers**.

## 1. Telemetry record v2 (CSV, self-describing header)

The header line (already sent first and every 200 records) defines the columns. The host (`server.py`) already adopts any header starting with `bmc_id`, so adding columns does not break parsing; only the hard-coded defaults and `app.js` need updating. Empty = not valid / not available (never a fake zero).

| Column | Units | Source | ICD / FSR item |
|---|---|---|---|
| `bmc_id` | text | config | — |
| `seq` | count | stamped at publish; advances even if dropped | gap detection |
| `t_us` | µs | `esp_timer`, boot-relative, stamped at **sample time** | ICD §6.3 (stamp at sampling) |
| `run_ms` | ms | since `PROFILE START` (or mode entry in manual runs); empty outside a run | ICD Table 6 "ms since run start" |
| `mode` | enum string | state machine | ICD "active control mode" |
| `plate_c` | °C | TMP117 on spreader plate | ICD "heater temperature" → **renamed**, D1 |
| `inlet_c` | °C | inlet TMP117 (when fitted) | FSR 3.2.4.1 ambient per run |
| `v48_v` | V | INA238 bus voltage | ICD Table 6 |
| `i48_a` | A | INA238 current (averaged) | ICD Table 6, feedforward input |
| `power_w` | W | INA238 | existing |
| `v33_v`, `v5_v`, `v12_v` | V | VRM monitor | FSR 3.2.1.6 (reserved; empty until VRM exists, A7) |
| `heater_cmd_pct` | % | commanded by profile/control | ICD "commanded heater PWM duty" |
| `heater_pct` | % | applied after arbitration (existing field) | |
| `fan_demand_pct` | % | control law output (RPM-reference percent) | |
| `fan_target_rpm` | RPM | demand × `RPM_REF` | |
| `fan1_duty_pct`, `fan2_duty_pct` | % | applied | ICD "commanded fan PWM duty" |
| `fan1_rpm`, `fan2_rpm` | RPM | tach, per fan | ICD "per fan" |
| `setpoint_c` | °C | active setpoint | analysis |
| `ctl_p`, `ctl_i`, `ctl_ff` | % | PID P / I terms and feedforward term | analysis and tuning |
| `ff_variant` | id | active feedforward variant | proactive comparison |
| `fault_code` | enum | latched first cause (0 = none) | ICD "fault status" |
| `fault_mask` | bitfield | currently-active fault conditions | ICD "fault status" |
| `flags` | bitfield | existing `TELEM_FLAG_*` plus new bits below | |
| `drops` | count | telemetry drops on the BMC | continuity |

New `flags` bits (existing bits 0–7 unchanged): `8 SELFTEST_OK`, `9 SAFE_TO_HANDLE`, `10 PROFILE_RUNNING`, `11 OT_WARN`, `12 TEST_HOOKS_ACTIVE`, `13 OT_LIMIT_LOWERED`.

`fault_code` enum: `0 NONE`, `1 OVER_TEMP`, `2 TEMP_LOSS`, `3 TELEM_LOSS`, `4 I2C_FAULT`, `5 FAN_STALL`, `6 SENSOR_STALE`, `7 RESET_WDT`, `8 RESET_BROWNOUT`, `9 RESET_PANIC`. Detail (device id / fan index) is carried in the `EVENT` record.

`fault_mask` bits: one per `fault_code` 1–6.

Size/rate: ~230 bytes at 10 Hz is ≈ 2.3 kB/s — far under Wi-Fi or SPI-Ethernet limits. The send buffer in `bmc_stream.c` (`line[160]`) must grow to ~320.

Compatibility: keep the v1 column *names* that still exist (`temp_c` → `plate_c` is the one rename; do it in one commit across firmware, `server.py`, `simulate_bmc.py`, `app.js`).

## 2. Event records

Events are the failsafe log (FSR 3.2.5.1.4) and self-test report. They travel on the same UDP stream as a line starting with `EVENT,`:

```
EVENT,<event_id>,<t_us>,<run_ms>,<type>,<code>,<detail...>
```
`<event_id>` is a separate counter (1, 2, 3 … per boot), not the telemetry `seq`; hosts de-duplicate the three repeats on it.

Types: `FAILSAFE_ENTER`, `SAFE_TO_HANDLE`, `ACK_OK`, `SELFTEST`, `MODE_CHANGE`, `RUN_START`, `RUN_END`, `RESET`, `WARN`.

Examples:
```
EVENT,1042,18340221,,FAILSAFE_ENTER,OVER_TEMP,plate_c=120.3 limit=120.0 mode=REACTIVE
EVENT,1043,18340221,,FAILSAFE_ENTER,FAN_STALL,fan=2 duty=62 rpm=0
EVENT,2201,31120000,,SELFTEST,FAIL,tmp117_plate: no ack @0x48
EVENT,5001,90000000,0,RUN_START,REACTIVE,profile=1 setpoint=70 kp=.. ki=.. kd=.. ff_variant=0 rpm_ref=7000 ot_limit=120
```

Rules:
- Each event is sent **three times** 100 ms apart (UDP loss), with the same `seq` so the host de-duplicates. The latched state in every telemetry record is the backstop.
- The BMC keeps the last 16 events in RAM and replays them on a `EVENTS` command, so a dashboard that connected late can still show what happened.
- `RUN_START` writes all control parameters into the stream so every dataset is self-describing and reproducible (supports the "tuning fairness" threat to validity in CONOPS §5.2).
- Host change: `server.py` currently counts any line whose fields don't match the header as malformed. It must route `EVENT,` lines to a separate handler, log them to `logs/events_<ts>.csv`, and push them to the browser.

## 3. Command protocol (host → BMC, UDP, text lines)

Format: `<CMD> [args]\n`. The BMC replies to the sender with one datagram: `OK <CMD> [info]` or `ERR <CMD> <reason>`. Commands carry an optional leading token `#<n>` (a host counter); the BMC echoes it in the reply so the host can retry safely. All commands are idempotent (re-sending is harmless).

| Command | Legal in | Effect |
|---|---|---|
| `HELLO` | any | Subscribe (existing) |
| `STATUS` | any | Reply with mode, selftest_ok, fault_code, plate_c, profile state |
| `EVENTS` | any | Replay last 16 events |
| `SELFTEST` | STANDBY | Run self-test (§ in `02`) |
| `MODE <STANDBY\|CHAR\|REACTIVE\|PROACTIVE>` | per `01` rules | Request mode change; `ERR` with reason if not allowed |
| `ACK` | FAILSAFE | Clear latch if the §4 rules in `02` are met |
| `HEATER <on> <pct>` | CHAR only | Existing manual lease command (host must resend < 1 s) |
| `FAN <pct>` | CHAR only | Manual fan demand (RPM-reference percent) |
| `SETPOINT <c>` | STANDBY, or REACTIVE/PROACTIVE with no run active | Plate setpoint; rejected during a run (fairness) |
| `GAINS <kp> <ki> <kd>` | STANDBY, or REACTIVE with no run active | Reactive PID gains (see `04`); rejected during a run |
| `FF <variant>` | STANDBY, or PROACTIVE with no run active | Select feedforward variant |
| `PROFILE CLEAR` / `PROFILE ADD <seconds> <heater_pct>` | STANDBY | Build a profile (max 16 steps) |
| `PROFILE PRESET <n>` | STANDBY | Load built-in profile. Preset 1 = CONOPS Scenario 4.1 (60 s idle, 600 s full, 300 s idle) |
| `PROFILE START` | an actuating mode, `selftest_ok` | Zero `run_ms`, emit `RUN_START`, begin timeline |
| `PROFILE STOP` | any | Abort profile, heater to 0, fans per mode (`STOP` is always accepted) |
| `TESTSET OT_LIMIT <c>`, `TESTINJECT <fault>` | test-hook builds only | See `02` §7 |

Security/robustness:
- Commands are accepted only from current subscribers (a HELLO within the last 10 s).
- Line length limit 64 bytes, strict parsing, unknown commands get `ERR`.
- `PROFILE STOP`, `MODE STANDBY` and the heater-off form of `HEATER` are always honored when legal; nothing a remote sender can do can *increase* heat outside the state-machine rules.
- No authentication is a documented limitation of the lab rack.

Host-side change list (`dashboard/`): add `POST /api/cmd` that relays a command and returns the reply; add buttons for self-test, mode, profile, ACK; show the state banner from `mode`, `fault_code` and `flags`; render events; keep the existing heater switch for CHARACTERIZATION; add the continuity checker (expected vs received count, monotonic `t_us`, empty-field counter).

## 4. GPIO map (WROOM-32E dev kit; S3 pins are decided at the port)

Existing (from `bmc_board.c`): LED 12, I2C SDA 22 / SCL 21, fan PWM 18, fan tach 19, fan enable 16, heater command PWM 23, heater permit 17.

To add (proposed; verify against boot-strapping pins and the dev kit before wiring):

| Signal | Direction | Notes |
|---|---|---|
| Fan 2 PWM, Fan 2 tach | out / in (PCNT) | independent per D10; second LEDC channel + PCNT unit |
| Heater enable | out | pull-down; replaces the stubbed `board_heater_enable_set` |
| I2C SDA/SCL sampling | in (same pins, read as GPIO after a failed transfer) | stuck-low detection (F4) |
| Fault LED (optional) | out | latched failsafe indicator on the sled (does not violate "no display/keypad" in a meaningful way, but check ICD §5.3) |

Avoid GPIO 0, 2, 5, 12, 15 (boot strapping) for anything that can be driven at reset; note GPIO 12 is already the LED today.

## 5. I2C map (addresses currently in `main.c`; the ICD map is still TBD)

| Device | Address | Poll | Required for heat? |
|---|---|---|---|
| TMP117 (spreader plate) | 0x48 | 10 Hz | **yes** |
| INA238 (48 V sense) | 0x40 | 10 Hz | yes (feedforward and power) |
| TMP117 (inlet, planned) | 0x49 (ADD0→V+) | 10 Hz | yes for run logging (FSR 3.2.4.1) |
| VRM rail monitor | TBD (Chiemeka's design) | 10 Hz | yes once it exists |
| SSD1306 OLED | 0x3C | 1 Hz | no |
| fan-link peer (emulation) | 0x44 | on change | no; remove when emulation ends |
| Raspberry Pi 5 | reserved | — | **deferred** |

Action: write this table into the ICD's Table 7 once part selection finishes; check fixed-address collisions between same-family parts (ICD §6.4 requirement). Use a bus-pull-up value chosen against measured harness capacitance before the sled harness is built (ICD §5.2.1).
