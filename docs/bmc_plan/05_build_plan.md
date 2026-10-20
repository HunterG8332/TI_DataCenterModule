# 05 — Build Plan

Ordering principle: **nothing that applies heat is written before the thing that stops the heat has been built and forced to trip.** Every phase ends in a demonstrable result, is committed on its own branch, and can be rolled back by reverting one merge. Phases P0–P6 need no heater hardware at all.

Effort figures are rough sessions (one session ≈ half a working day) and are my estimate, not a commitment. Dates reference the Execution Plan (today 2026-10-19; ECEN 403 final report due 2026-12-11).

## Working agreements (to keep it low-stress)

1. One branch per phase (`phase/p2-failsafe`), merged to `main` only when its exit criteria pass. `main` always builds and always boots to a safe state.
2. Tag each green phase (`bmc-p2-done`). If a later phase misbehaves, flash the previous tag.
3. Constants first: before the logic, put thresholds in `bmc_config.h` (P0). Debugging a threshold should never mean hunting through files.
4. Test-hook builds (`CONFIG_BMC_TEST_HOOKS`) are for validation only; the demo build turns them off.
5. Keep a one-page **bench log** (date, firmware tag, what was connected, result) per session. It becomes the evidence for the validation plan rows.
6. Update the dashboard in the same phase as the firmware change it depends on, using `simulate_bmc.py` so you can test the host without hardware.
7. Do not mix refactors and behavior changes in one commit.

## Phase overview

| Phase | Name | Heat? | Closes (validation rows) | Est. | Suggested target |
|---|---|---|---|---|---|
| P0 | Baseline, config header, doc fixes | no | — | 1 | 10/23 |
| P1 | Split tasks, sensor snapshots, single-writer actuators | no | — | 3–4 | 10/30 (matches "firmware architecture" milestone) |
| P2 | Mode state machine + failsafe core (dummy heater) | no | 3.2.5.1.2 (part), .3 (part), .4, .5, 3.2.4.3 (logic) | 4–5 | 11/6 |
| P3 | I2C robustness + bus-fault detection | no | 3.2.5.1.3 (I2C) | 2–3 | 11/10 |
| P4 | Command channel + telemetry v2 + dashboard | no | 3.2.3.2.3, 3.2.1.6 (part), 3.2.3.2.2 | 4–5 | 11/17 |
| P5 | Self-test | no | 3.2.5.1.1 | 2 | 11/20 |
| P6 | Second fan + RPM-matching layer + fan characterization | no | 3.2.3.2.4 | 4 | 11/24 |
| P7 | Real heater path + profile executor | **yes (staged)** | 3.2.1.3, 3.2.1.4, 3.2.3.1.2/.3 (data), 3.2.1.2 (data) | 4–6 + bench time | when gate driver exists (Execution Plan: 10/30 design, 11/13 heater bring-up) |
| P8 | Characterization, plant ID, reactive PID | yes | 3.2.1.5 (reactive half) | 5–6 | 12/4 |
| P9 | Proactive framework + feedforward variants | yes | 3.2.1.5 (proactive half) | 5–6 | 404 (Spring) |
| P10 | Integration, soak, demo rehearsal | yes | 3.2.3.1, 3.2.5, full demo | 5+ | 404 |

Suggested **ECEN 403 cut line:** P0–P6 plus the P7 bench steps that the hardware allows. P8 starts when the plate sensor and real fans are in place. P9–P10 carry into 404. If hardware slips, P0–P6 still produce a complete, demonstrable BMC safety and telemetry story for the final report.

---

## P0 — Baseline, config header, doc fixes

Tasks
- Tag the current commit `bmc-baseline`.
- Create `components/bmc_config/include/bmc_config.h`; move every constant now in `main.c` (periods, fan curve, stall, heater timeout) and add the new ones from `02` §1. Replace `HEATER_MAX_TEMP_C = FAN_CURVE_HIGH_C` with `OT_TRIP_C` (120) and `OT_WARN_C`.
- Read `tmp117.c` and `ina238.c`; write down their actual conversion/averaging configuration (see README). Decide the TMP117 mode deliberately (continuous, minimal averaging, or one-shot per cycle) and note the effective update rate.
- Fix the stale GPIO comment in `bmc_board.c`.
- Send the document-discrepancy list (`00_README.md`) to the teammates.

Exit: builds, behavior unchanged, constants centralized, TMP117 effective rate known and recorded.
Rollback: revert the merge; nothing else depends on it yet.

## P1 — Split tasks, snapshots, single-writer actuators

Tasks
- `bmc_sensors`: sensor task publishes a `sensor_snapshot_t` {plate, ina, (inlet), seq, per-field `valid`, `age`, `consecutive_failures`}. Move OLED to its own low-priority task.
- `bmc_actuators`: owns heater PWM/enable and fan outputs; API `set_heater(pct)`, `set_fan_duty(i, pct)`, `force_safe()`. Heater enable pull-down; implement the real `board_heater_enable_set` (still driving an LED/scope in S0).
- Move heater arbitration out of `fan_task` into the actuator filter. `fan_task` keeps the current fan curve for now (behavior unchanged) but only proposes a duty.
- Replace loose `volatile` globals with the snapshot interface.
- Stack high-water-mark logging; add jitter logging on the sensor loop (baseline measurement).

Exit: everything the firmware does today still works (dashboard shows the same data); jitter histogram over 10 minutes saved; no `volatile` shared control state remains.
Rollback: revert to `bmc-p0-done`.

## P2 — Mode state machine and failsafe core (dummy heater)

Tasks
- `bmc_safety`: safety task per `01`/`02`: fault table F1, F2, F5 first (over-temp, temp loss, fan stall), then the rest as their phases land; latch, first-fault rule, cooldown hold, `SAFE_TO_HANDLE`, ACK rules, `FAILSAFE_ENTER` event.
- Modes `STANDBY` and `FAILSAFE` only at first, plus a temporary `CHAR` mode that takes the existing `HEATER` command so today's dashboard keeps working. `REACTIVE`/`PROACTIVE` exist as names that reject entry until their phases.
- Test hooks: `TESTSET OT_LIMIT`, `TESTINJECT`.
- Task watchdog subscription.
- Heater output goes to the LED only (S0).

Exit (all recorded in the bench log):
1. Boot lands in STANDBY, heater off, from power-up, from WDT reset and from brown-out (pull the USB power).
2. Force each of F1, F2, F5 with the hooks and with the physical method: warm the TMP117 with a lowered limit, unplug the TMP117, unplug a fan; the LED turns off and the fan goes to 100 % within the stated latency each time.
3. While latched, `MODE CHAR` and `HEATER` are refused; `ACK` is refused while the plate is above 60 °C or the fault persists; `ACK` works when safe, and the state returns to STANDBY with `selftest_ok = false`.
4. Event record has cause + timestamp and appears in the dashboard log.

Rollback: revert to `bmc-p1-done`.

## P3 — I2C robustness and bus-fault detection

Tasks
- `bmc_i2c_xfer()` wrapper: per-device consecutive-failure counters, timeouts, bus-busy handling; after a failed transfer, sample SDA/SCL as GPIO to detect stuck-low.
- Bus recovery sequence (9-clock toggle, then re-init) used in STANDBY; in an actuating mode a stuck bus is simply `I2C_FAULT` failsafe.
- Wire F3 and F4 into the fault table; classify bus-root-cause vs single-device.

Exit: injected faults — unplug INA238 mid-run (F3), unplug TMP117 (F2), pull SDA low with a jumper through a resistor (F4) — each produces failsafe with the right cause and device in the event; reconnecting does not clear the latch.

## P4 — Command channel, telemetry v2, dashboard

Tasks
- `bmc_cmd` parsing per `03` §3 (UDP replies, `#n` echo, subscriber-only).
- Telemetry v2 record and `EVENT` lines; grow buffers.
- `server.py`: `POST /api/cmd`, event handling and `events_*.csv`, new default columns, continuity report (expected vs received, monotonic `t_us`, empty-field counts). `simulate_bmc.py` and `app.js` updated: state banner, mode/self-test/ACK buttons, event list, per-fan RPM.
- Rename `temp_c` → `plate_c` across all four files in one commit.

Exit: from the dashboard alone you can run STANDBY → (self-test stub) → CHAR → heater step → force a fault → see the event → ACK. A 10-minute run shows `drops` and gap counters at 0 and fields populated. Unplugging Wi-Fi mid-run does not change control timing (jitter log).

## P5 — Self-test

Tasks: `bmc_selftest` per `02` §5; wire to `SELFTEST`; add `selftest_ok` gating to all actuating-mode entries.

Exit: healthy → PASS. Then, in turn: plate sensor unplugged, INA238 unplugged, fan 1 unplugged, fan 2 unplugged, tach wires swapped, fan enable open → each FAILS naming the source, and each blocks `MODE CHAR`.

## P6 — Second fan, RPM matching, fan characterization

Tasks
- Instantiate `bmc_fan` twice (second LEDC channel, second PCNT unit); per-fan RPM and stall detection; per-fan fields in telemetry.
- `bmc_fanctl`: target RPM from demand %, per-fan `duty_map` + PI (see `04` §2). Start with the maps empty (pure PI) and fill them from the sweep.
- Sweep tool: run 0–100 % in 10 % steps per fan, log duty/RPM, produce the per-fan table and the achievable RPM ceiling. Choose `RPM_REF`.
- Decide tach window for the inner loop.

Exit: sweep CSV for both fans (monotonic, independent), RPM-matched operation holds both fans within ±X RPM (set X from tach resolution; propose 3 %) across 20–100 % demand; stalling either fan trips failsafe.

## P7 — Real heater path and profile executor (staged heat)

Prerequisite hardware: gate driver and MOSFET (Execution Plan: "Heater gate drive and MOSFET design"), heater element, current-limited 48 V supply, plate sensor mounted, guard/label on hot surfaces.

Tasks
- Heater PWM at the chosen frequency (ties to INA238 averaging; `02`/`01`), enable line, permit switch in series with the real path.
- INA238 configured as 48 V current/voltage with the averaging window fixed; calibrate against a bench meter and record the offset/gain error.
- Profile executor on the BMC (`PROFILE …` commands, preset 1 = Scenario 4.1), `RUN_START`/`RUN_END` events, `run_ms`.
- Follow the S0→S4 staged procedure in `02` §6. Each stage's pass criterion is a recorded test.

Exit: validation rows 3.2.1.3 (100 W for 10 min at full fan with plate below the limit), 3.2.1.4 (0/25/50/75/100 % × 3 sweeps, repeatability), and the data for 3.2.3.1.2/.3 and 3.2.1.2 are captured, with failsafe re-tested with the real heater (lowered OT threshold).

## P8 — Characterization, plant identification, reactive PID

Tasks
- CHARACTERIZATION mode finished (manual heater + fan RPM, holds and logs).
- Host scripts: step-response logger, FOPDT fit, SIMC calculator, overlay metrics.
- Reactive PID per `04` §3–4; identify at 2–3 RPMs; choose gains; verify with setpoint step and Scenario 4.1 profile.
- Enforce start-condition check (plate within tolerance of inlet) in `PROFILE START`.

Exit: Reactive-mode run of Scenario 4.1 completes, repeats within a stated tolerance, produces the baseline overshoot/time-above-setpoint numbers; tuning report written.

## P9 — Proactive framework and feedforward variants

Tasks: `ff_variant_t` registry, 2–3 variants, `FF`/`FFPARAM` commands, PI trim shared with reactive, logging of `ctl_ff`; characterization tables from P8 data; paired runs vs reactive.

Exit: all selectable at runtime without reset (validation 3.2.1.5: mode field in log + observed fan behavior + no controller reset); paired datasets captured.

## P10 — Integration, soak, demo rehearsal

Tasks
- Full demo script: install in enclosure, self-test, reactive run, return to ambient, proactive run, inject a fault live.
- Soak: ≥ 1 hour of repeated profiles, count false trips, drops, resets.
- Input-robustness test (3.2.3.1): out-of-order commands, power sequences, unplug/replug.
- Optional: persist the failsafe latch in NVS so a power cycle cannot silently clear it (assumption A4).
- Port to ESP32-S3 + W5500 (D4) with a re-run of the safety regression (the P2/P3/P5 injection tests become a regression suite).

Exit: validation matrix (`06`) fully populated with evidence; documentation set (operator manual, design record) complete.

---

## Risk register (BMC firmware)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| TMP117 default 1 s conversion makes the reactive baseline artificially slow | High until checked | High (invalidates experiment fairness) | Check in P0; set mode explicitly; document |
| Hardware late (gate driver, heater, sensors) | Medium | Schedule | P0–P6 are hardware-independent by design |
| Fan RPM-matching inner loop interacts with stall detector | Medium | Nuisance trips | Characterize first (P6); keep stall detector on measured RPM and duty floor, not target |
| Shared-state races after task split | Medium | Subtle bugs | Snapshot structs, sequence stamps, jitter and stack logging in P1 |
| False failsafe trips during long runs | Medium | Lost runs | Debounce, cause-coded events, soak test in P10 |
| ESP32-S3 port surprises | Low–Medium | Rework | Pins only in `bmc_board`; regression suite from P2/P3/P5 |
| Scope creep into Pi interface | — | Time | Explicitly deferred |
| Documents and firmware drift apart | High | Validation confusion | Resolve the list in `00_README.md` in the next FSR/ICD revisions |

## Definition of done for the BMC (feeds the validation plan)

1. Every row in `06_verification_matrix.md` has a recorded result with a firmware tag and bench-log reference.
2. Every fault in `02` §2 has been forced at least once in the production build or in a test-hook build whose behavior is identical, with the failsafe log kept.
3. A paired reactive/proactive dataset exists with run-start metadata embedded.
4. The demo can be run end-to-end from the dashboard without a debugger or a reflash.
