# 01 — Firmware Architecture

## Design rules (these are what keep the build low-stress)

1. **Single writer for actuators.** Exactly one component (`bmc_actuators`) touches the heater PWM, heater-enable GPIO and fan PWM outputs. Everything else *proposes*; the safety task *decides*. This is what makes "independent of the active control law" (FSR 3.2.5.1.2) true by construction instead of by discipline.
2. **Safe by default.** Reset, brown-out, task crash, or an unwired pin must all produce *heater off, fans on/idle*. Heater enable has a pull-down; permit and fan-enable pins have pull-ups (already true today).
3. **Safety path is small and boring.** No heap allocation, no blocking calls, no sockets, no logging in the trip path (log *after* actuators are made safe). Target well under 200 lines for the trip logic.
4. **Control laws are replaceable plug-ins** that take a sensor snapshot and return a fan demand (and, in profile runs, nothing about the heater — the profile owns the heater). They cannot reach hardware.
5. **Network never blocks control** (already true: `telemetry_publish` is non-blocking; keep it that way).
6. **Every behavior has a config constant** in one header (`bmc_config.h`) — thresholds, periods, fan limits — so tuning and the S3 port don't touch logic.

## Component map

Existing components stay. New ones are marked **NEW**.

```
main/                     app_main: init order, task creation only
components/
  bmc_board/              pins + board-specific constants (port point for S3)
  bmc_config/       NEW   bmc_config.h: all thresholds/periods (single source of truth)
  bmc_i2c/                bus init  → add bmc_i2c_xfer() wrapper w/ failure counting + recovery
  tmp117/ ina238/ ssd1306/  device drivers (unchanged API)
  bmc_sensors/      NEW   sensor_task: polls devices, stamps age/validity, consecutive-fail counters
  bmc_fan/                one-fan PWM+tach driver (instantiate twice)
  bmc_fanctl/       NEW   RPM-matching layer: demand% → target RPM → per-fan duty (04)
  bmc_heater_cmd/         heater PWM output driver (unchanged; owned by bmc_actuators)
  bmc_actuators/    NEW   single-writer output layer + safety filter + failsafe override
  bmc_safety/       NEW   safety_task, fault table, mode state machine, latch, events
  bmc_selftest/     NEW   self-test sequence (called by safety task)
  bmc_control/      NEW   reactive PID + proactive (pluggable feedforward) + profile runner
  bmc_cmd/          NEW   parses UDP commands, queues them to safety task, builds replies
  bmc_telemetry/          queue + drop counter (extend record, see 03)
  bmc_stream/             UDP telemetry out + command in (extend, see 03)
  bmc_net/                SoftAP now, W5500 later
  bmc_fan_link/           I2C fan-duty peer for sensor emulation (remove when emulation ends)
```

## Tasks, cores, priorities, rates

Core 1 is the control core; core 0 is network (existing convention).

| Task | Core | Priority | Period | Does |
|---|---|---|---|---|
| `safety` | 1 | **10** (highest app) | 50 ms | Reads snapshots, evaluates fault table, runs mode state machine, drives `bmc_actuators`, handles commands from queue, feeds task watchdog |
| `sensors` (today `tmp117`) | 1 | 8 | 100 ms (`vTaskDelayUntil`) | TMP117 / INA238 (/ future VRM, inlet) reads, validity + age + fail counters, publishes snapshot, calls `telemetry_publish` |
| `control` | 1 | 6 | 100 ms (on new sample) | Runs active control law, profile runner; outputs *proposals* (fan demand %, heater %) to `bmc_actuators` |
| `fanctl` | 1 | 5 | 50 ms | Inner RPM loops (per fan), tach reads, stall detection inputs |
| `net_tx` / cmd rx | 0 | 5 | 50 ms service | Existing stream task; also receives commands, forwards to `bmc_cmd` queue |
| OLED | within `sensors` today | — | 1 Hz | Move to its own low-priority task so I2C OLED traffic cannot delay sensor reads |

Notes:
- Today `fan_task` does fan curve, stall detection, I2C fan-link send, heater arbitration and tach reads in one loop. In P1 that is split so *arbitration moves to the safety task* and *tach/stall detection stays with the fan layer but only reports*; the safety task decides what a stall means.
- Shared data crosses tasks as **immutable snapshots with a sequence stamp** (a small struct copied under a short critical section, or a double-buffer), not as loose `volatile` globals. Each field carries `valid`, `age_ms`, and `consecutive_failures`.
- Stack sizes are re-checked with `uxTaskGetStackHighWaterMark` in P1 and logged once at the end of every run.
- The task watchdog (TWDT) subscribes `safety`, `sensors` and `control`. A hang resets the chip into STANDBY with heater off, and the reset reason is reported (see `02`, F6).

## Data flow

```
 TMP117 plate ─┐
 INA238 48V   ─┼─> sensors ──snapshot──> safety ──final outputs──> bmc_actuators ──> heater PWM/enable
 (inlet, VRM)  ┘      │                    ▲   │                                └──> fan PWM x2
 tach x2 ─> fanctl ───┘ (RPM, stall flags) │   └─ events/state ─> telemetry ─> UDP ─> host
                                            │
 control (PID / FF / profile) ─proposals────┘          host ─UDP cmd─> bmc_cmd ─queue─> safety
```

The safety task is the only consumer of commands. Mode changes, ACK, SELFTEST and profile start/stop all go through it, so they can be validated against the current state in one place.

## Mode state machine

Exactly one mode at a time (CONOPS §3.4). `FAILSAFE` has priority over every transition.

| Mode | Heater | Fans | Entered by | Left by |
|---|---|---|---|---|
| `STANDBY` | off | idle policy (off / floor) | boot, ACK of failsafe, end of self-test, MODE STANDBY | MODE cmd |
| `SELFTEST` | forced off | exercised one at a time | SELFTEST cmd (from STANDBY only) | completion → STANDBY (sets or clears `selftest_ok`) |
| `CHARACTERIZATION` | fixed manual step (HEATER cmd with lease, or profile) | fixed manual duty or RPM | MODE CHAR (needs `selftest_ok`) | MODE cmd, failsafe |
| `REACTIVE` | profile-driven | PID on plate temp | MODE REACTIVE (needs `selftest_ok`) | MODE cmd, failsafe |
| `PROACTIVE` | profile-driven | feedforward + PI trim | MODE PROACTIVE (needs `selftest_ok`) | MODE cmd, failsafe |
| `FAILSAFE` | **off** | **100 % duty, both fans, open-loop** | any fault in `02` from any mode | ACK, only when safe |

Transition rules:
- Boot always enters `STANDBY`. Never directly into an actuating mode.
- Actuating modes (`CHARACTERIZATION`, `REACTIVE`, `PROACTIVE`) require: `selftest_ok`, no latched fault, heater permit closed for heater output, fan enable interlock closed.
- Switching between actuating modes is allowed only while **no profile is running** and the heater is at 0 % (this protects the "same profile, matched conditions" experiment).
- Any detected fault → `FAILSAFE` in the same safety-task iteration (≤ 50 ms), regardless of what the control law or a command said.
- `FAILSAFE` is left only by `ACK` accepted under the rules in `02` §4; after ACK the machine returns to `STANDBY` with `selftest_ok = false`, so a full self-test must pass before heat can be applied again.
- Commands that are illegal in the current state are rejected with `ERR <reason>` and change nothing (this is also the evidence for validation row 3.2.3.1 and 3.2.5.1.5).

## Heater path (what changes from today)

Today `heater_arbiter()` decides in the fan task and the "heater" is an analog-demand PWM to an emulator; `board_heater_enable_set()` is a stub. Target:

- `bmc_actuators_set_heater(pct, reason)` is the only function that can raise heater output. It clamps to `HEATER_MAX_PCT_*` (configurable per phase of bring-up, see `02` §6) and refuses unless the state machine, permit pin and fan-OK allow it.
- `bmc_actuators_force_safe()` sets heater 0, enable low, both fans 100 %. It is what the safety task calls on failsafe, and it does not take any lock the control task could hold.
- The existing hardware permit pin and fan-enable pin remain as independent inhibits — firmware can only ever reduce heat further than they allow.
- Heater PWM frequency and the INA238 averaging window are tied by the ICD §5.2.2 constraint (average over ≥ 1 heater PWM period; heater PWM ≫ telemetry rate). Fix them together when the gate driver is designed; keep them as `HEATER_PWM_HZ` and `INA_AVG_*` config constants.

## Timing budget (10 Hz, D9)

| Item | Budget |
|---|---|
| Sensor cycle | 100 ms period, jitter logged. TMP117 read ≈ 1 ms at 400 kHz; INA238 ≈ 1 ms; budget ≤ 10 ms for all devices |
| Safety evaluation | ≤ 1 ms per 50 ms iteration |
| Worst-case fault-to-safe latency | 3 failed reads × 100 ms + ≤ 50 ms = **≈ 350 ms** for telemetry-type faults; ≤ 50 ms after an over-temp reading; ≤ 50 ms + tach window (250–500 ms) for stall |
| Telemetry | one record / 100 ms; any record that cannot be sent increments `drops` |

Jitter measurement (Stage 2 in the original learning plan) should be run once the split is done and kept as a baseline: it is also part of the final dataset's credibility.

## What is deliberately not in the design

- No local SD/flash logging (FSR: stream to host).
- No auth on the command channel (lab rack, bench supply is the real E-stop). Documented as a known limitation; bind commands to the HELLO subscribers only.
- No automatic failsafe recovery, ever.
- No BMC-to-Pi interface (deferred by request).
