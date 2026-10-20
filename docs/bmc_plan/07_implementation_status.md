# 07 — Implementation Status (branch `feature/bmc-state-machine`)

Written after the first implementation pass. It records what now exists, where it differs from documents 01–06, how it was checked, and — most importantly — **what has not been verified on hardware**.

## 1. What is built

| Phase in `05` | Status | Where |
|---|---|---|
| P0 config header, TMP117/INA238 check | Done | `components/bmc_core/include/bmc_config.h`; TMP117 was already in fast continuous mode (≈15.5 ms, no averaging), so the lag risk in `00` item "verify" did not exist |
| P1 task split, snapshots, single-writer actuators | Done (jitter/stack-watermark logging **not** done) | `components/bmc_app/` (`sensors.c`, `supervisor.c`, `actuators.c`, `bmc_aux.c`; not named `aux.c`, which Windows reserves as a device name and Git cannot add) |
| P2 mode state machine + failsafe core | Done | `bmc_core/bmc_safety.c`, `bmc_brain.c` |
| P3 I2C robustness | Done | per-device failure counters, stuck-line detection, bus recovery, re-init after unplug (`sensors.c`, `bmc_i2c.c`, drivers) |
| P4 command channel, telemetry v2, dashboard | Done | `bmc_cmd.c`, `bmc_fmt.c`, `bmc_stream.c`, `dashboard/` |
| P5 self-test | Done | `bmc_core/bmc_selftest.c` |
| P6 second fan, RPM matching | Done in firmware; **fan maps/gains need measuring** | `bmc_fanrpm.c`, `actuators.c`, `supervisor.c` |
| P7 profile executor | Done; **real heater gate path not done** | `bmc_profile.c`; heater output is still the analog "demanded power" PWM plus the enable line |
| P8 reactive PID | Done; **gains are placeholders** (no plant identification yet) | `bmc_pid.c`, `bmc_brain.c` |
| P9 feedforward framework | Done (4 variants, runtime selectable); **parameters are placeholders** | `bmc_ff.c`, `FF` / `FFPARAM` commands |
| P10 integration, soak, S3 port | Not started | |

## 2. How it was checked

| Check | Command | Result |
|---|---|---|
| Portable logic: 494 unit/system checks under AddressSanitizer + UBSan (safety state machine, every fault, latch/ACK rules, PID, RPM matching, stall detector, profiles, feedforward, command parser, formatters, self-test failure cases, heater arbitration, closed loop against a simulated thermal plant) | `make -C firmware/BMC_firm/test_host test` (Linux/WSL gcc) | 0 failures |
| Firmware compiles and links with ESP-IDF v6.1 | `idf.py build` in `firmware/BMC_firm` | clean; app 0xcdb00 bytes, 20 % of the partition free |
| Dashboard server + simulator end to end (commands, refusals, failsafe, event de-duplication, ACK rules, continuity stats, logs, `--no-control`) | `python dashboard/test_integration.py` | 29 checks, 0 failures |
| Dashboard JavaScript | `node --check dashboard/static/app.js` | syntax OK (not exercised in a browser) |

**Nothing has been flashed or run on the board.** Everything in section 5 below is still to be proven on the bench, with the dummy heater first.

## 3. Differences from documents 01–06

| # | Plan said | Implementation | Why |
|---|---|---|---|
| 1 | `bmc_config` component | Header lives in `bmc_core/include/bmc_config.h` | One component the host tests can compile |
| 2 | Separate `control` and `fanctl` tasks | Control law (10 Hz) and the fan RPM loops run inside the supervisor task | Fewer cross-task races; the law is pure code that cannot reach hardware, and the safety verdict is applied last |
| 3 | OLED in sensor task | OLED and the fan-link peer moved to a low-priority `aux` task | A missing peripheral can no longer delay a sensor read |
| 4 | All faults trip failsafe in every mode | **Over-temp trips in every mode; the others only in CHAR/REACTIVE/PROACTIVE** | In STANDBY there is no heat, and the self-test must be able to run with a sensor unplugged to name it |
| 5 | Command replies sent by the supervisor | Replies and events go through queues to the net task | The safety task never calls the network stack (a `sendto` can block on the TCP/IP thread) |
| 6 | Event line starts with the telemetry `seq` | First field after `EVENT` is a separate **event id** | De-duplication of the three repeats |
| 7 | Self-test cross-talk threshold in absolute RPM | Idle fan must stay under 50 % of the driven fan's RPM; swapped tach wires get a named message | Fans idle-spin on their own |
| 8 | TWDT on idle tasks | TWDT 1 s on the supervisor and sensor tasks only; panic on timeout | A hung safety task must reset the chip quickly; Wi-Fi load must not be able to |
| 9 | STANDBY fans "idle" | STANDBY / SELFTEST use the old linear temperature curve (0 % at 25 °C, 100 % at 100 °C) | Keeps passive cooling and today's bench behaviour |
| 10 | Telemetry columns in `03` | Also keeps `fan_en`; 31 columns total (checked equal to the header by a unit test) | Existing dashboard semantics |
| 11 | Heater held on a 1–2 failed-read blip | A momentarily unreadable plate temperature forces heater 0 immediately, before the 3-read trip | Never heat blind, even briefly |
| 12 | `HEATER` commands | `HEATER 1 …` only in CHAR (else `ERR HEATER mode`); `HEATER 0 …` accepted silently in any mode | The dashboard sends the keep-alive every 250 ms |
| 13 | Heater enable on a new pin | `GPIO27`, mirrored to the LED on `GPIO12` while no real heater exists (`BMC_HEATER_ENABLE_MIRRORS_LED`) | Dummy-heater bench test needs something visible. **Both are proposals — change in `bmc_board.c`** |
| 14 | Test hooks off by default | `CONFIG_BMC_TEST_HOOKS=y` in `sdkconfig` for validation; every record carries flag bit 12 while on | Needed to run the validation plan; switch off for the demo build |

## 4. Open decisions and placeholders you must confirm

- **Pins:** fan 2 PWM/tach `GPIO25/26`, heater enable `GPIO27` (`bmc_board.c`). Fan 1 stays `18/19`; one enable interlock `GPIO16` gates both fans.
- **Which fan is which:** fan 1 is assumed to be the 7 000 RPM ARCTIC; fan 2 the 10 000 RPM part (`BMC_FAN1_MAX_RPM`, `BMC_FAN2_MAX_RPM`). `BMC_RPM_REF` = 7 000: at 100 % demand fan 1 needs 100 % duty. If it cannot reach 7 000 RPM, lower `BMC_RPM_REF` (its loop otherwise saturates).
- **Fan duty→RPM maps** default to linear (duty = rpm / max). Measure each fan's sweep and replace the tables (`bmc_fanrpm_t.map_*`); the PI trims the difference meanwhile. Tach pulses per revolution is assumed 2 for both fans.
- **Placeholders needing calibration:** `BMC_PID_*` gains, `BMC_FANRPM_KP/KI`, all feedforward parameters, `BMC_SELFTEST_AMBIENT_*` (10–40 °C), `BMC_SELFTEST_REQUIRE_48V` (0 until the 48 V supply feeds the INA238), `BMC_HEATER_MAX_PCT_BRINGUP` (100 now; lower it in code at each heat stage).
- **`SAFE_CLEAR_C` = 60 °C** and **`OT_TRIP_C` = 120 °C** are set per your answer; both documents (FSR/ICD) still say 120 °C for both.
- **Failsafe fan speed:** both fans 100 % duty open loop (assumption A1 in `00`).
- **INA238** still measures whatever is wired to it; current/voltage readings are only meaningful for a 2 mΩ shunt on the 48 V path.

## 5. Bench checklist (stage S0: dummy heater only — do this before ANY 48 V)

Setup: flash with `idf.py -p COMx flash monitor`; join the `BMC-Telemetry` Wi-Fi; `python dashboard/server.py`; open `http://localhost:8090`. The **LED on GPIO12 is the dummy heater** (it shows the heater-enable line). Close the permit switch (GPIO17 to ground) and the fan-enable interlock (GPIO16 to ground). Keep a log in the bench notebook with the firmware commit.

| # | Test | Do | Expect |
|---|---|---|---|
| T1 | Boot | Power up | Serial: `BMC up: STANDBY`; LED off; dashboard banner STANDBY; both fan RPM columns present |
| T2 | Reset safety | Pull power while LED is on (use T5 first), restore | Boots into STANDBY, LED off, no heating |
| T3 | Self-test, healthy | Click *Run self-test* | ≈7 s; fan 1 then fan 2 spin; event `SELFTEST PASS`; modes become available |
| T4 | Self-test failures | One at a time: unplug plate TMP117; unplug INA238; unplug fan 2 (or its tach); swap the two tach wires; open the fan-enable interlock | Each gives `SELFTEST FAIL` naming the source (`tmp117_plate:no_response`, `ina238:no_response`, `fan2:no_tach…`, `tach_swapped_with_fan2…`, `fan_interlock:open`); *Characterization* stays disabled. Re-plug and re-run: passes (proves re-init after unplug) |
| T5 | Manual heater | Self-test, *Characterization*, manual heater switch on, 40 % | LED on. Open permit switch → LED off. Open fan interlock → LED off. Close the browser tab → LED off within about 1 s |
| T6 | Over-temp trip | In Characterization with heater on: `TESTSET OT <ambient+4>` then warm the plate TMP117 with a finger | Failsafe: LED off, both fans 100 %, event `FAILSAFE_ENTER OVER_TEMP` with plate temperature and timestamp, banner red. Scope the enable pin: low within about 50 ms of the over-limit reading |
| T7 | Sensor loss | In Characterization, unplug the plate TMP117 | Failsafe `TEMP_LOSS` within about 0.4 s |
| T8 | Telemetry source loss | Unplug the INA238 | Failsafe `TELEM_LOSS` |
| T9 | I2C bus fault | Hold SDA to ground through ≥1 kΩ | Failsafe `I2C_FAULT` (the bus fault outranks the device losses it causes) |
| T10 | Fan stall | In Characterization, *FAN 50* via the console command box, then stop a fan (unplug power or pin the rotor) | Failsafe `FAN_STALL` within about 0.7 s; record which fan, and whether disconnecting the tach vs the power looks different |
| T11 | Latch | After any trip: try *Characterization*, *Acknowledge* | Refused (`latched` / `not_safe`); heater stays off; fans stay at 100 % |
| T12 | Cooldown and ACK | Clear the forced faults; wait | `SAFE_TO_HANDLE` 5 s after the plate is below 60 °C; *Acknowledge* then returns to STANDBY with the self-test flag cleared; *Characterization* refused until a new self-test passes. (To see the hold with a cold bench, heat the TMP117 above 60 °C with hot air first.) |
| T13 | Profile | *Characterization*, preset 2, *Load + start* | LED follows the 0 % / 50 % / 0 % timeline (10 s, 30 s, 20 s); `RUN_START` and `RUN_END complete` events; PROFILE STOP aborts instantly; a trip mid-run aborts it (`RUN_END failsafe`) |
| T14 | Closed loop, fans only | *Reactive* with the heater profile at 0 (profile preset 2 with no permit is fine); warm the sensor with a finger | Fan demand rises with temperature; **fan 1 and fan 2 RPM columns stay equal** |
| T15 | RPM matching | In Characterization: `FAN 20`, `FAN 40`, `FAN 60`, `FAN 80`, `FAN 100` | Both fans track the same RPM (target = demand × 7 000); note where fan 1 saturates |
| T16 | Fan sweep for the maps | `FAN 0` to `FAN 100` in steps of 10, 10 s each | Per-fan duty→RPM data from the telemetry log, to replace the default maps |
| T17 | Continuity | Run 10 minutes streaming | Dashboard: lost 0, dropped 0, longest gap ≈0.1 s, out-of-order stamps 0, records missing a field 0 |
| T18 | Network isolation | Switch the host off or the Wi-Fi off during T14 | Fans/closed loop keep running; no change in behaviour |

## 6. Not done / not verified

- Anything on real hardware (section 5).
- Jitter histogram and stack high-water-mark logging (P1 asked for them).
- Hang test hook (`TESTINJECT STALE` exercises the stale-data logic but not a real task hang or the watchdog reset path).
- Real heater gate/MOSFET PWM path, heater PWM frequency and INA238 averaging decisions (ICD 5.2.2).
- VRM rail voltage telemetry (columns reserved and empty), inlet sensor (compile option `BMC_INLET_SENSOR`, untested).
- NVS persistence of the failsafe latch across a power cycle (a reboot lands in STANDBY and logs the reset reason).
- Host analysis scripts (FOPDT fit, SIMC calculator, run-overlay metrics).
- Ethernet/ESP32-S3 port; BMC-to-Pi interface (deferred by request).
- The dashboard was not opened in a browser by the author; only its server API and JavaScript syntax were checked.
