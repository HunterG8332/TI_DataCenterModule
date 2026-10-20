# 06 — Verification Matrix (BMC)

Maps each row of the Validation Plan (Draft A) that the BMC firmware participates in to the feature that satisfies it, the phase that builds it, the test procedure the firmware needs to support, and the evidence to store. Rows owned entirely by hardware (mechanical, VRM rails, fan count) are listed at the end for completeness; the BMC only contributes telemetry to them.

Status column values follow the validation plan: UNTESTED / PARTIALLY TESTED / PASSED / FAILED. Current state is as of 2026-10-19.

## BMC-owned or BMC-dependent rows

| FSR § | Test | Today | Firmware feature | Phase | Procedure (firmware-specific additions) | Evidence to keep |
|---|---|---|---|---|---|---|
| 3.2.1.2 | Load current telemetry rate | UNTESTED | INA238 read at 10 Hz as `i48_a`; sweep of sample rate configurable in `bmc_config.h` | P7 (data), P0 (config) | Sweep 10 Hz → 1 kHz INA conversion/averaging settings against a known load; confirm accuracy vs meter; confirm 10 Hz in the log (`t_us` deltas) | Sweep CSV, meter readings, `t_us` delta histogram |
| 3.2.1.3 | Emulated thermal load capacity | UNTESTED | Real heater path, 100 % hold, fans 100 %, plate monitored, S0–S4 stages | P7 | 10 min at 100 %; compute `v48_v × i48_a` for the heater branch; plate stays below `OT_TRIP_C` | Log with `power_w`, `plate_c`, events |
| 3.2.1.4 | Load step control and repeatability | UNTESTED | Profile executor with 0/25/50/75/100 % steps; heater PWM | P7 | Three sweeps; compare delivered power per duty across sweeps | 3 sweep logs + comparison table |
| 3.2.1.5 | Control mode selection | UNTESTED | Reactive PID, proactive FF + PI trim, `MODE` command, `mode` column | P8, P9 | Run the same short profile in each mode from the host with no reset between; confirm `mode` field and fan behavior | Logs with `mode`, `ff_variant`, reset counter unchanged |
| 3.2.1.6 | Telemetry logging continuity | UNTESTED (seq/drops/gap-detect exist) | Telemetry v2, host continuity report | P4 | Full profile; check monotonic `t_us`, no gap > 1 sample interval, no empty required field, count = rate × duration | Continuity report from `server.py` |
| 3.2.3.1 | Input signal robustness | UNTESTED | State-machine rejection of illegal commands; safe boot; permit/interlock pins | P2, P10 | Out-of-order commands, power-up/down in odd orders, random unplugging; confirm reject or safe handling and no damage | Command/reply transcript, bench log |
| 3.2.3.2.2 | Telemetry output | UNTESTED | UDP stream, host-side received vs sent counts | P4 | `seq` + BMC `drops` vs host received/written count | Host report |
| 3.2.3.2.3 | Diagnostic output | UNTESTED | Command channel + dashboard controls | P4 | Exercise each function: mode change, profile start/stop, ACK refused until safe, live values update | Screenshots + command transcript |
| 3.2.3.2.4 | Fan control output | PARTIALLY TESTED | Per-fan PWM at 25 kHz, per-fan PCNT tach, RPM-matching layer | P6 | Sweep duty 0–100 % in 10 % steps, record duty and RPM per fan; monotonic; fans independent | Per-fan sweep CSVs + plot |
| 3.2.4.1 | Operating inlet temperature | UNTESTED | `inlet_c` in telemetry; `PROFILE START` guard against out-of-range start | P4/P8 | Record inlet per run; check 18–27 °C | Logs |
| 3.2.4.3 | Surface temperature and burn hazard | UNTESTED | Failsafe holds fans 100 % until plate < `SAFE_CLEAR_C` (60 °C) for 5 s, `SAFE_TO_HANDLE`, ACK gating | P2 | Force trip with lowered OT threshold, observe fans 100 %, `SAFE_TO_HANDLE` at 60 °C, ACK refused before | Log + event records + thermocouple/IR reading |
| 3.2.5 | Fault containment | UNTESTED | `force_safe()` + failsafe from any fault; hardware permit and fan-enable | P2, P3, P10 | Each injected fault leaves heater off and fans 100 %; confirm no damage to supply/host | Fault-injection table below |
| 3.2.5.1.1 | Initialization self-test | UNTESTED | `bmc_selftest` | P5 | Healthy → pass; then sensor disconnected, fan disconnected, swapped tach, interlock open → each blocks actuating modes and names the source | Event records + replies |
| 3.2.5.1.2 | Over-temperature cutoff | UNTESTED | Safety-task F1, independent of control law | P2, P8/P9 | `TESTSET OT_LIMIT` lowered under load; heater off, fans 100 %; repeat in reactive and proactive | Two logs (one per law) |
| 3.2.5.1.3 | Fault detection coverage | UNTESTED | F1–F5 | P2, P3 | Inject each of: over-temp; telemetry device disconnect mid-run; SDA held low; fan stall (disconnect) | Fault-injection table below |
| 3.2.5.1.4 | Failsafe logging | UNTESTED | `EVENT,FAILSAFE_ENTER` with cause + timestamp, host event log | P2, P4 | For each injection above, one event naming the right cause with timestamp near injection | `events_*.csv` |
| 3.2.5.1.5 | Failsafe latching and recovery | UNTESTED | Latch, ACK rules, `selftest_ok` reset on ACK | P2 | After failsafe try each actuating mode (refused); ACK refused until safe; ACK then self-test then normal operation | Transcript + log |
| N/A | Full system demonstration | UNTESTED | Everything | P10 | Per Validation Plan methodology | Final dataset + demo video |

## Rows the BMC does not own (BMC contribution in brackets)

3.2.1.1 Rail regulation accuracy [`v33/v5/v12_v` telemetry once VRM monitor exists]; 3.2.2.1 Volume envelope; 3.2.2.2 Fan size and count [firmware drives ≥ 2 fans from P6]; 3.2.2.3 Mounting; 3.2.3.1.1 Input voltage [`v48_v` telemetry]; 3.2.3.1.2 Power consumption [`power_w`]; 3.2.3.1.3 Input current level [`i48_a`]; 3.2.3.2.1 Regulated output rails [rail telemetry]; 3.2.4.2 Airflow direction and repeatability [RPM-matched fans help repeatability].

## Fault-injection plan (used by 3.2.5.1.2, .3, .4, .5 and 3.2.4.3)

Run each injection from a healthy state in an actuating mode (heater at a low, safe level), in both the test-hook build and, where physically possible, the production build. Record a bench-log line per run.

| # | Fault | Physical injection | Software injection (test hooks) | Expected | Check |
|---|---|---|---|---|---|
| I1 | Over-temperature | warm plate with heater, with limit lowered | `TESTSET OT_LIMIT <c>` just above the current temperature | `OVER_TEMP`, heater off, fans 100 % ≤ 50 ms after sample | log, event |
| I2 | Plate sensor loss | unplug plate TMP117 | `TESTINJECT temp_loss` | `TEMP_LOSS` in ≈ 350 ms | event time vs unplug time |
| I3 | Other telemetry loss | unplug INA238 | `TESTINJECT telem` | `TELEM_LOSS` + device id | event |
| I4 | I2C bus fault | hold SDA to ground through a series resistor (≥ 1 kΩ, 3.3 V logic) | `TESTINJECT i2c` | `I2C_FAULT` | event |
| I5 | Fan stall | disconnect fan (power or tach, record which) | `TESTINJECT stall1` / `stall2` | `FAN_STALL` + fan index | event |
| I6 | Task hang | n/a | build-only test that blocks the sensor task | `SENSOR_STALE` or WDT reset → STANDBY + `RESET_WDT` event | event |
| I7 | Reset / brown-out | pull USB/power | n/a | boot into STANDBY, heater off | log |
| I8 | Latch behavior | n/a | n/a | After each of I1–I5: actuating-mode commands refused; ACK refused until `SAFE_TO_HANDLE` and fault clear; ACK then self-test required | transcript |

Pass rule: for every injection, (a) heater output reads 0 within the stated latency, measured on a scope on the heater PWM/enable line, not only from the log, (b) both fans at 100 % duty, (c) exactly one `FAILSAFE_ENTER` event with the right cause and a timestamp consistent with the injection, (d) the latch persists until ACK.

## Status tracking

Keep a copy of this table in the repo and update the Today column with `PASSED <firmware tag> <date>` as rows close, then feed the result back to the shared Validation Plan (currently 26 UNTESTED, 1 PARTIALLY TESTED, 0 PASSED).
