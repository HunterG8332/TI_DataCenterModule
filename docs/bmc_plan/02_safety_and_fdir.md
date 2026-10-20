# 02 — Safety and FDIR

Governing requirements: FSR 3.2.4.3, 3.2.5, 3.2.5.1.1–3.2.5.1.5; ICD §4 and §6.4.

## 1. Thresholds and constants (all in `bmc_config.h`)

| Constant | Value | Notes |
|---|---|---|
| `OT_TRIP_C` | **120.0 °C** | Spreader plate. Hard maximum; runtime override may only *lower* it (see §7) |
| `OT_WARN_C` | 110.0 °C | Flag only (no action) — gives the operator and the log a lead |
| `SAFE_CLEAR_C` | **60.0 °C** | Safe-to-handle threshold (D2). Needs to be confirmed against the actual plate material and guard design |
| `SAFE_CLEAR_HOLD_S` | 5 s | Plate must stay below `SAFE_CLEAR_C` this long before "safe" is reported |
| `TEMP_VALID_MIN_C / MAX_C` | −20 / 150 °C | Reading outside this is treated as a sensor fault, not a temperature (TMP117 range is −55…150) |
| `FAIL_READS_TRIP` | 3 | Consecutive failed reads per required device (D7) |
| `SENSOR_STALE_MS` | 500 | No new sensor snapshot for this long → sensor task presumed hung |
| `FAN_STALL_MIN_DUTY_PCT` | 10 % | Existing; below this a fan may legitimately idle |
| `FAN_STALL_WINDOW_MS` | 250 ms | Existing; consider 500 ms (RPM resolution, see `04`) |
| `SELFTEST_AMBIENT_MIN/MAX_C` | 10 / 40 °C | Assumption A5 |
| `HEATER_REQ_TIMEOUT_MS` | 1000 | Existing lease for the manual HEATER command |
| `FAILSAFE_FAN_DUTY_PCT` | 100 | Assumption A1 |

## 2. Fault table

Every row below results in `FAILSAFE` unless noted. "Detect" is what the safety task evaluates every 50 ms.

**Arming (as implemented):** F1 over-temperature trips in **every** mode. F2–F6 trip only in an actuating mode (CHARACTERIZATION, REACTIVE, PROACTIVE). In STANDBY and SELFTEST there is no heat to remove, and the conditions must remain observable so the self-test can run with a sensor unplugged and name it; they are still reported in `fault_mask`. Entry to any actuating mode requires a passed self-test, so heat can never start while one of these conditions is present.

| ID | Condition | Detect | Latency (worst) | Cause code | Notes |
|---|---|---|---|---|---|
| F1 | Over-temperature | plate `valid` and `temp ≥ OT_TRIP_C` (single sample) | ≤ 50 ms after the sample | `OVER_TEMP` | Independent of mode and control law |
| F2 | Plate sensor loss | plate TMP117 fails `FAIL_READS_TRIP` consecutive reads, **or** reading outside valid range | ≈ 350 ms | `TEMP_LOSS` | "Operating blind" = same as over-temp (FSR rationale) |
| F3 | Other telemetry source loss | INA238 (and future required devices: inlet sensor, VRM monitor) fail `FAIL_READS_TRIP` consecutive reads | ≈ 350 ms | `TELEM_LOSS` + device id | Only *required* sources trip; OLED is not required |
| F4 | I2C bus fault | all devices failing together, **or** SDA/SCL stuck low (GPIO sample after a failed transfer), **or** driver timeout/bus-busy | ≈ 350 ms, or immediately if line stuck low | `I2C_FAULT` | Takes precedence over F2/F3 as the reported cause when the bus is the root cause |
| F5 | Fan stall | existing detector: fan enabled, duty ≥ floor for ≥ 1 tach window, RPM = 0, **per fan** (D11) | window + ≤ 50 ms | `FAN_STALL` + fan index | |
| F6 | Control-system hang / reset | task watchdog expiry; or `sensors` snapshot older than `SENSOR_STALE_MS` | WDT timeout; ≤ 500 ms | `SENSOR_STALE`, or `RESET_WDT` after reboot | A reset lands in STANDBY with heater off; the event is recorded after boot |
| F7 | Brown-out / panic reset | `esp_reset_reason()` at boot | at boot | `RESET_BROWNOUT`, `RESET_PANIC` | Informational event, no latch (A4) |

Non-failsafe conditions (inhibit + flag only):

| Condition | Response |
|---|---|
| Heater permit pin open | Heater held at 0 %, flag `HEATER_PERMIT` clear (existing behavior) |
| Fan enable interlock open | Fan held at 0 %, heater inhibited, flag `FAN_DISABLED` (assumption A3) |
| Manual HEATER lease expired (host silent > 1 s) | Heater 0 %; does **not** abort a BMC-run profile (D8) |
| `OT_WARN_C` crossed | Flag only |
| Telemetry send failure | `drops++` (never affects control) |

## 3. Failsafe behavior

On entry (inside the safety task, **before** any logging or networking):

1. `bmc_actuators_force_safe()`: heater PWM = 0, heater-enable low, both fans 100 % duty open-loop.
2. Latch `fault_code` (first cause) and `fault_mask` (all active conditions); record `t_us`.
3. Abort any running profile.
4. Queue an `EVENT` record (see `03`) and set the fault bits in every telemetry record from now on, so an operator who misses the one-shot event still sees the latched state.
5. Mode = `FAILSAFE`; commands other than `ACK`, `STATUS`, `MODE STANDBY`-rejections are refused with `ERR latched`.

First-fault rule: later faults add to `fault_mask` but never overwrite `fault_code`.

## 4. Latch, cooldown, clear (FSR 3.2.4.3, 3.2.5.1.5; decision D6)

- Fans stay at 100 % until the plate has been `< SAFE_CLEAR_C` for `SAFE_CLEAR_HOLD_S`. Then the BMC sets flag `SAFE_TO_HANDLE` and reports it in telemetry and as an event ("safe handling temperature reached").
- `ACK` (dashboard button) is accepted only if **all** of:
  1. `SAFE_TO_HANDLE` is set,
  2. the plate sensor is currently valid (we must *see* that it is cool),
  3. no fault condition is currently active (e.g. the sensor is back, the fan is turning, bus healthy),
  4. no profile is running (always true in failsafe).
  Otherwise the reply is `ERR <first unmet condition>` and nothing changes.
- A successful `ACK` returns the BMC to `STANDBY` with `selftest_ok = false`. A full self-test must pass before any heat can be applied again (this re-verifies the faulted source).
- If the failed element *is* the plate sensor, the BMC cannot prove the plate is cool, so it stays latched. The only way out is to repair/reconnect it, or power-cycle (A4); the operator manual should say so.
- ACK is never automatic and never time-based.

## 5. Initialization self-test (FSR 3.2.5.1.1)

Triggered by `SELFTEST` from `STANDBY` only. Heater is forced off throughout. ≈ 10 s total. Result is reported as one `EVENT SELFTEST` record listing every check, plus a `PASS`/`FAIL <source>` reply.

| # | Check | Pass criterion | Failure message names |
|---|---|---|---|
| 1 | I2C enumeration | every address in the expected list ACKs (`TMP117 plate`, `INA238`, later inlet TMP117 / VRM monitor) | the address and device |
| 2 | Temperature plausibility | each temperature sensor reads between `SELFTEST_AMBIENT_MIN_C` and `_MAX_C` | sensor and the value read |
| 3 | 48 V sense plausibility | INA238 reads a bus voltage within ±10 % of 48 V *when* `SELFTEST_REQUIRE_48V` is set (off for bench work without the supply), current near idle | value read |
| 4 | Fan exercise, one at a time | drive fan *n* to a test duty for ~2 s: its RPM rises above a minimum, **and** the other fan's tach stays near 0 (catches swapped tach wires and checks "each fan reports independently") | fan index and reason |
| 5 | Interlocks readable | fan-enable pin closed (otherwise the fan cannot be exercised) | `fan interlock open` |
| 6 | Heater output is off | commanded heater is 0 and the enable line reads low | `heater not off` |

Any failure leaves `selftest_ok = false` and blocks entry to every actuating mode. Passing sets `selftest_ok = true`; it is cleared by any failsafe, any reboot, and an ACK.

Why plausibility rather than "device responds": a connected-but-garbage TMP117 is the most dangerous case in the project (heat with no valid feedback), and this is what catches it.

## 6. Bring-up safety procedure (what protects the hardware and the people)

Heat is introduced in stages. Each stage has a **stop condition** and a **rollback** (power the heater off at the bench supply; revert the firmware commit).

| Stage | Heater load | Firmware heater cap | Pass to next stage when |
|---|---|---|---|
| S0 | none; heater output drives an LED / scope only (today's setup) | n/a | Every row of the fault table has been forced and trips as specified (see `06`, fault-injection section) |
| S1 | heater element **disconnected** from 48 V, gate driver powered, PWM scoped | 100 % | Gate waveform matches duty; enable low at reset/power-up; failsafe drops it within 50 ms |
| S2 | 48 V from a **current-limited supply** (≤ 0.5 A), element in place | 10 % | Plate temperature rises as predicted by the thermal budget; sense current matches the multimeter; someone is watching the plate |
| S3 | current limit 2 A | 40 % | Plate stable at predicted temperature with both fans RPM-matched; OT trip tested at a *lowered* threshold |
| S4 | full limit (4–5 A) | 100 % | 10-minute 100 % hold for validation 3.2.1.3 with fans at full |

The cap is a config constant (`HEATER_MAX_PCT_BRINGUP`) enforced in `bmc_actuators`, changed by a code edit at each stage, never by a command.

Bench rules:
- Operator present at all times; one hand near the bench-supply output switch for the first power-up of each stage.
- Guard or label the plate and heatsink before S2 (FSR 3.2.4.3).
- No stage is skipped because a previous stage "looked fine"; each exit criterion is a recorded test.
- Keep the permit switch wired in series with the real heater path as well as read by firmware, so there is a human-operable inhibit that does not depend on code.

## 7. Test hooks needed to validate the safety path

The validation plan requires forcing faults (lower threshold, stall a fan, hold SDA low, disconnect a sensor). Some are physical; two need firmware support, compiled only when `CONFIG_BMC_TEST_HOOKS=y`:

- `TESTSET OT_LIMIT <c>`: lowers `OT_TRIP_C` at runtime. The firmware refuses values above the 120 °C constant and the setting is reported in telemetry so a log never hides that it was used. Resets to 120 on reboot and on ACK.
- `TESTINJECT <fault>`: forces a fault condition (`temp_loss`, `i2c`, `stall1`, `stall2`) to exercise logging when the physical injection is not convenient.

Production builds (`CONFIG_BMC_TEST_HOOKS=n`) reject both commands. The final demo build is the production build.

## 8. Failure modes of the safety design itself

| Risk | Mitigation |
|---|---|
| Safety task starved or blocked | Highest app priority, no blocking calls, TWDT subscribed, I2C reads are in other tasks |
| Shared-state race between tasks | Snapshot structs with sequence stamp; safety only trusts data with `age_ms < SENSOR_STALE_MS` |
| Control law bug commands heat | Actuator layer clamps and gates every proposal; the law has no pointer to hardware |
| Heater stuck on because an output driver crashed | Heater-enable pull-down; the PWM channel is stopped by `force_safe`; the hardware permit remains |
| False trips make the experiment unusable | `FAIL_READS_TRIP` debounce; all trips logged with cause so tuning is data-driven; soak test in P10 |
| Latch cleared by mistake | ACK conditions in §4 are checked by the BMC, not the dashboard |
| Fan wires swapped (tach/PWM) | Self-test check 4 |
