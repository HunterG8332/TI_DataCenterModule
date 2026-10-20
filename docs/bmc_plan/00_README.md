# BMC Build Plan — Overview and Decision Log

Status: **Draft A** — written 2026-10-19 from the Concept of Operations (Rev 4), FSR (Rev 1), ICD (Rev 1), Execution Plan and Validation Plan (Draft A), and the current firmware on `main` (commit 4414405).

Scope: everything the BMC firmware and its host dashboard must do, **except the BMC-to-Raspberry-Pi interface**, which is deliberately out of scope until the team is ready.

## Purpose

The firmware today is a working sensing / fan / telemetry / heater-arbitration slice. It is not yet the BMC the CONOPS describes, because it has no mode state machine, no latched failsafe, no self-test, no control laws, no command channel, and only one fan. This plan describes how to build the rest **safely** (nothing that applies heat is written before the thing that stops the heat) and **without stress** (small phases, each ending in something demonstrable, each with a rollback).

## Document set

| File | Contents |
|---|---|
| `00_README.md` | This file: decisions, assumptions, open items, doc discrepancies |
| `01_architecture.md` | Tasks, ownership of actuators, mode state machine, data flow, timing |
| `02_safety_and_fdir.md` | Fault table, thresholds, latch/clear rules, self-test, bring-up safety procedure |
| `03_interfaces.md` | Telemetry v2 record, event records, fault codes, command protocol, GPIO and I2C maps |
| `04_control_laws.md` | RPM-matching fan layer, reactive PID (OpenBMC-style cascade), tuning method, feedforward framework |
| `05_build_plan.md` | Phases P0–P10 with tasks, exit criteria, rollback, rough schedule |
| `06_verification_matrix.md` | Every validation-plan row mapped to the feature, phase, procedure and evidence |
| `07_implementation_status.md` | What is built on `feature/bmc-state-machine`, deviations from this plan, placeholders to calibrate, the S0 bench checklist, what is unverified |
| `08_information_flow.md` | How data moves: the two cores, every queue between them, telemetry/command/failsafe paths, the SoftAP link, and the built-in expansion points |

## Decisions already made (from the planning questions)

| # | Decision | Chosen |
|---|---|---|
| D1 | Control variable for the reactive law | **Spreader plate** temperature (TMP117 mounted on the plate). The ICD says "heat sink" in places and must be corrected. |
| D2 | Over-temp cutoff / clear threshold | **Cutoff 120 °C, clear (safe-to-handle) 60 °C** — separate values with wide hysteresis. |
| D3 | Where safety logic runs | **Dedicated safety task**, highest priority, sole owner of the final actuator outputs. |
| D4 | Development hardware | **ESP32-WROOM-32E dev kit now, port to ESP32-S3 later.** Board abstraction (`bmc_board`) keeps the port to one file. |
| D5 | Command channel | **Extend the existing UDP text protocol** (MODE, SELFTEST, ACK, PROFILE …) with reply datagrams. |
| D6 | Fault acknowledgement | **Dashboard button only**, accepted only when safe (see `02`). |
| D7 | I2C / telemetry-loss trip | **3 consecutive failed reads** (≈300 ms at 10 Hz). |
| D8 | Load profile execution | **On the BMC.** Host uploads or selects a profile; BMC runs the timeline. |
| D9 | Sample rate (open item A-2) | **Keep 10 Hz** for all channels. |
| D10 | Fans | **Independent PWM + tach per fan.** Fans are different (≈10 k RPM max and ≈7 k RPM max); the BMC matches their **RPM**, using the 7 k fan as the reference ceiling. |
| D11 | Single-fan stall | **Failsafe on any stall.** |
| D12 | Reactive PID structure and tuning | **OpenBMC `phosphor-pid-control` structure** (cascade: thermal PID → target RPM → fan PID), tuned with a **documented step-response (FOPDT / SIMC) method**. OpenBMC specifies no tuning method; see `04`. |
| D13 | Feedforward form | **Undefined on purpose.** The firmware provides a pluggable feedforward interface so 2–3 variants can be tested and selected at runtime. |

## Assumptions the plan makes (confirm or correct)

| # | Assumption | If wrong |
|---|---|---|
| A1 | Failsafe drives **both fans to 100 % duty open-loop** (bypassing RPM matching), because the FSR says "full speed". In normal control modes the 10 k fan is capped to match the 7 k fan. | Change `failsafe_fan_policy` in config; no architectural impact. |
| A2 | Both fans produce 2 tach pulses/rev (current firmware constant). | Make pulses-per-rev per fan in `bmc_board`. |
| A3 | `fan enable` interlock open in an actuating mode = heater inhibit + warning flag (current behaviour), not a failsafe. | Add FAN_DISABLED to the failsafe table. |
| A4 | A reboot returns to STANDBY (heater off) and counts as clearing a latch, but the reset reason is logged. | Add NVS persistence of the latch (listed as optional P10 item). |
| A5 | Ambient plausibility window for self-test is 10–40 °C (wider than the 18–27 °C operating spec). | Edit `SELFTEST_AMBIENT_MIN/MAX_C`. |
| A6 | The INA238 channel is wired/configured as **48 V input current and bus voltage** by the time the real heater is connected. Today it reports `power_w` on whatever rail the bench uses. | Telemetry fields stay blank until valid. |
| A7 | VRM rail voltages (3.3 / 5 / 12 V) will be readable by the BMC over I2C once the VRM exists (FSR 3.2.1.6 requires them in the log). | Fields stay reserved/blank; test 3.2.1.6 cannot fully pass until then. |

## Open items and conflicts in the documents

These are inconsistencies found while reading the PDF. They should be resolved in the next document revision, because the validation plan inherits them.

1. **Cutoff = cooldown = 120 °C** (FSR 3.2.4.3, ICD §4). Resolved here as D2; the documents need the same change.
2. **ICD says "heat sink" temperature sensor** (§4) vs CONOPS "spreader plate". Resolved as D1.
3. **Heater power:** ICD §4 "up to 200 W", ICD §5.2.3 "≈150 W", FSR minimum 100 W.
4. **Input current:** validation plan "≈3.8 A", ICD "≈4 A".
5. **Fan count:** FSR requires ≥ 2 fans; firmware drives one.
6. **Fan max RPM mismatch (≈10 k vs ≈7 k):** not in any document. The ICD fan section should state the RPM-matching rule and the part numbers.
7. **ICD telemetry table:** "Heater temperature" must be renamed to plate temperature; timestamp is "ms since run start", firmware currently stamps µs since boot (v2 carries both).
8. **Failsafe "full speed" meaning** when fans are RPM-matched (assumption A1) should be stated in the FSR.
9. **Execution plan:** fan PWM / tach bring-up shows "Not Started" while the validation plan shows "Partially Tested". The status table in the PDF text is hard to read; verify row alignment in the source file.
10. **Stale comment** in `bmc_board.c` says the permit pin is GPIO27; the define is GPIO17.
11. **Heater over-temp ceiling is tied to the fan curve** (`HEATER_MAX_TEMP_C = FAN_CURVE_HIGH_C = 100 °C`). It must become an independent constant (120 °C cutoff) in `bmc_config.h`.

## Things to verify in the code before relying on them (not yet read)

- ~~**TMP117 conversion configuration**~~ **Checked (2026-10-19):** `tmp117.c` already writes continuous mode, no averaging (≈15.5 ms per result). The 1 s lag risk does not exist. Keep it in the design record because the ICD names sensor response time as a fairness concern.
- **INA238 configuration** (`ina238.c`): 2 mΩ shunt, 5 A full scale, ADC_CONFIG default (continuous, 1052 µs, no averaging), now written explicitly. The ICD requires the reported current to be an average over ≥ 1 heater PWM period; fix averaging together with the heater PWM frequency.
- **`bmc_net_ap.c`**: SoftAP only today; Ethernet is a later item.
- ~~`simulate_bmc.py`, `static/app.js` need updating~~ **Done** (see `07_implementation_status.md`).

> Implementation status, differences from this plan, open placeholders and the bench checklist are in `07_implementation_status.md`.
