# BMC Design Overview — Academic Data Center Module

2026-09-17 · @Someone

## 1. Scope and design assumptions

This document defines the Baseboard Management Controller (BMC) subsystem for the Academic Data Center Module. It covers only the **WILL** items in CONOPS Draft A, Section 3.3.4, plus the supporting interfaces those items require. MAY items (web dashboard, remote power control of the compute node, per-fan zoning, PMBus compliance) are deliberately absent from the architecture below, though nothing here forecloses them.

One deviation is recorded for sponsor visibility. The CONOPS lists a wired Ethernet uplink as **MAY**, with Wi-Fi as the committed transport. This design uses Ethernet as the primary host link because the selected board (Waveshare ESP32-S3-ETH) carries an onboard W5500 controller, making Ethernet available at no added schedule cost. Wi-Fi remains functional on the same socket code and requires no hardware change, so committed scope survives if the Ethernet path is dropped. This is a MAY item implemented early, not a WILL item added.

Fixed inputs taken from the CONOPS and treated as non-negotiable:

- 48 V DC input, \~175 W sled budget, ≥100 W emulated compute load, 5 A design point on the 48 V sense path
- Three VRM rails: 3.3 V, 5 V, 12 V, each within ±5% under full load
- Two PWM fans, ≤80 mm, front-to-back, with tachometer feedback
- Raspberry Pi 5 with Active Cooler, powered from the VRM 5 V rail
- I²C as the intra-sled telemetry bus (NXP UM10204), BMC as sole bus controller
- Six modes: Off/Standby, Initialization/Self-Test, Characterization, Reactive Cooling, Proactive Cooling, Failsafe
- Telemetry sampling and control at **20 Hz**, above the ≥10 Hz CONOPS floor

## 2. Hardware platform, power, and pin allocation

**Board.** Waveshare ESP32-S3-ETH (ESP32-S3R8: Xtensa LX7 dual-core at 240 MHz, 512 KB SRAM, 8 MB PSRAM, 16 MB flash). Ethernet is a W5500 controller on SPI with a hardware TCP/IP engine, so the MAC/PHY work does not load the CPU. The board exposes a Pico-compatible header.

**Power.** The BMC is fed 5 V from the VRM 5 V rail into the board's 5 V node; the onboard regulator derives 3.3 V. USB-C is used only for flashing and console, and the two sources are never connected at once. Budget 500 mA of headroom at 5 V. The BMC must be powered whenever the 48 V input is present, including in Failsafe, so its 5 V feed is taken ahead of any heater or compute enable.

**Voltage domains and references.** All BMC logic is 3.3 V. All measurement is done by I²C sensors with their own internal references; the ESP32-S3 SAR ADC is deliberately not used for any telemetry channel, because its accuracy and linearity are inadequate for a dataset the project intends to publish. Fan tachometer outputs are open-collector and are pulled to 3.3 V, never to 12 V. Fan PWM inputs are specified open-drain in the Intel 4-wire standard; the BMC drives them open-drain with a pull-up to the fan controller's own supply, which avoids back-driving 3.3 V into a 5 V input structure.

**Reserved pins.** GPIO9–14 are the W5500 (MISO 12, MOSI 11, SCLK 13, CS 14, RST 9, INT 10). GPIO4–7 are the TF card. GPIO33–37 are internally occupied and unusable. GPIO21 drives the onboard WS2812. GPIO43/44 are the UART0 console.

| Function | Pin | Peripheral | Notes |
| --- | --- | --- | --- |
| I²C SDA / SCL | GPIO16 / GPIO17 | I2C0 controller | 400 kHz, 2.2 kΩ pull-ups to 3.3 V |
| FAN1 PWM | GPIO18 | LEDC ch0 | 25 kHz, open-drain |
| FAN2 PWM | GPIO1 | LEDC ch1 | 25 kHz, open-drain |
| FAN1 / FAN2 tach | GPIO15 / GPIO38 | PCNT unit 0 / 1 | Pull-up to 3.3 V, RC filter |
| HEATER PWM | GPIO39 | LEDC ch2 | To gate driver, low-side N-FET |
| HEATER ENABLE | GPIO40 | GPIO out | Hardware AND with PWM; cleared in Failsafe |
| Pi UART TX / RX | GPIO42 / GPIO41 | UART1 | 115200 8N1, 3.3 V, common ground |
| INA238 ALERT | GPIO47 | GPIO in, interrupt | Open-drain, pulled up |
| TMP117 ALERT | GPIO48 | GPIO in, interrupt | Open-drain, pulled up |
| Status LED | GPIO21 | RMT | Mode indication |

**Heater drive.** The BMC does not switch 48 V directly. GPIO39 feeds a gate driver (a TI UCC27517-class single-channel low-side driver is the intended part) that drives the array's N-channel MOSFET. GPIO40 is an independent enable gated in hardware with the PWM, so the failsafe path can remove heater drive without depending on the PWM peripheral being in a known state.

## 3. Sensor and actuator interfaces

### 3.1 I²C telemetry bus

One bus, 400 kHz Fast-mode, 3.3 V, BMC as sole controller, 2.2 kΩ pull-ups sized for the bus capacitance of a single sled. No multi-controller arbitration, no clock stretching by design, no bus multiplexer. Every device is addressed directly and the address map is fixed at design freeze so a missing device is a self-test failure rather than an ambiguity.

| Device | Address | Measures |
| --- | --- | --- |
| INA238 #1 | 0x40 | 48 V input rail: bus voltage, current, power — **the feedforward signal** |
| INA238 #2 | 0x41 | 12 V rail |
| INA238 #3 | 0x44 | 5 V rail (Raspberry Pi 5 feed) |
| INA238 #4 | 0x45 | 3.3 V rail |
| TMP117 #1 | 0x48 | Inlet air |
| TMP117 #2 | 0x49 | Outlet air |
| TMP117 #3 | 0x4A | Spreader plate (control variable) |
| TMP117 #4 | 0x4B | Heat sink fin stack |

**Power monitor recommendation: INA238.** The 48 V sense path is the deciding constraint. The INA238 tolerates an 85 V common-mode input, so it can sit high-side on the 48 V rail; the INA226 (36 V) cannot, and mixing families would mean two drivers and two calibration procedures. Using one part on all four rails gives a single driver, one register map, and one error budget. For the 48 V rail at 5 A, a 5 mΩ Kelvin-connected shunt yields 25 mV full-scale current signal, inside the ±40.96 mV ADCRANGE setting and dissipating 125 mW at 5 A. Configuration: ADCRANGE = 1, bus and shunt conversion times set so a full conversion completes well inside the 50 ms control period, with light hardware averaging. The INA238's internal ADC reference means no external voltage reference part is required anywhere in the telemetry chain.

**Temperature: TMP117.** ±0.1 °C without calibration, 7.8125 m°C resolution, and four selectable addresses (0x48–0x4B) via the ADD0 pin. Four addresses is exactly four sensors, which is the design limit; a fifth sensor would require a second bus or a different part, and this constrains sensor BOM decisions (CONOPS open item A-3). Sensors are configured continuous-conversion with 8-sample averaging and read asynchronously to their conversion cycle. Spreader-plate and heat-sink sensors are the thermally slow channels; inlet/outlet sensors are in the air stream and must be shielded from radiant heat off the sink.

### 3.2 Fans

Two four-wire 80 mm fans on the VRM 12 V rail, commanded as one zone (per-fan control is a MAY item). PWM is generated by LEDC at **25 kHz** with 11-bit duty resolution, which is the practical maximum resolution at that frequency from the 80 MHz source clock and is well above the audible band. A minimum duty floor of roughly 20% is enforced in firmware so the commanded speed never falls below the fan's stall threshold.

Tachometer returns two pulses per revolution and is counted by the PCNT peripheral over a 250 ms gate, giving RPM = pulses × 120 and a 120 RPM quantization. Tach is a health signal, not a control input: it feeds stall detection and the self-test, and is logged alongside the duty command so fan energy can be estimated afterward. A fan reporting zero RPM while commanded above the duty floor for three consecutive windows is a stall.

### 3.3 Heater array

The BMC produces variable thermal load by **low-frequency PWM** of the constantan array through the gate driver, at 100 Hz with 12-bit duty resolution. At 100 Hz the thermal mass integrates completely, so the plate sees a smooth power level rather than pulses, while the switching rate stays low enough that gate-drive losses and wiring inductance effects are negligible. Commanded load is expressed to the operator as a percentage of full array power; actual delivered power is computed from the INA238 on the 48 V rail rather than assumed from duty, which is what makes the load profile reproducible between runs.

### 3.4 Raspberry Pi 5 link (recommended: UART)

The Pi reports its own state to the BMC over UART1 at 115200 8N1, 3.3 V, TX/RX crossed with a common ground and no flow control. A small Python service on the Pi emits one line per second containing CPU package temperature, load average, and throttle status; the BMC parses it, timestamps it, and forwards it to the host as informational telemetry. It is never an input to a control law.

This is preferred over putting the Pi on the I²C bus. A Linux host as an I²C target is awkward to make reliable, and any clock stretching or driver hiccup would appear as a fault on the same bus the control loop depends on. UART keeps the failure domains separate: if the Pi is absent, hung, or rebooting, the BMC sees stale lines and keeps running, and the self-test treats the Pi link as informational rather than blocking.

## 4. Firmware architecture

**Framework: ESP-IDF v5.x**, chosen over Arduino because the BMC needs explicit control over task priorities, core affinity, and watchdog configuration. FreeRTOS is used directly; no Arduino compatibility layer is linked.

### 4.1 Component layout

```
main/                 app_main: init, self-test sequencing, mode machine
components/
  bmc_board/          pin map, board constants — the only file that changes per board
  bmc_i2c/            bus init, transaction helpers, error counters
  ina238/             register map, calibration, current/voltage/power reads
  tmp117/             register map, averaging config, temperature reads
  bmc_fan/            LEDC duty, PCNT tach, duty floor, stall detection
  bmc_heater/         gate PWM, enable line, profile execution
  bmc_control/        PID, feedforward, mode-selectable control step
  bmc_safety/         trip evaluation, latch, failsafe actuation
  bmc_net/            W5500 bring-up, UDP telemetry, TCP command server
  bmc_proto/          record formatting, command parse, ACK generation
  bmc_cfg/            NVS-backed configuration (gains, setpoint, IP, run metadata)
```

All pin numbers live in `bmc_board`. Porting the development work from an ESP32-WROOM-32E bring-up board to the ESP32-S3-ETH is then a single-file change plus a target switch.

### 4.2 Tasks and core allocation

| Task | Core | Priority | Period | Role |
| --- | --- | --- | --- | --- |
| `ctrl_task` | 1 | 20 | 50 ms | Acquire all I²C telemetry, run control law, command fan and heater, publish sample |
| `safety_task` | 1 | 22 | 50 ms | Independent trip evaluation; can assert Failsafe regardless of control state |
| `net_tx_task` | 0 | 10 | queue-driven | Drain sample queue, format, send UDP |
| `cmd_task` | 0 | 9 | blocking | TCP accept and command handling |
| `pi_uart_task` | 0 | 8 | blocking | Parse Pi status lines |
| `tach_cb` | 1 | ISR | 250 ms gate | PCNT overflow and gate handling |

Core 1 holds the deterministic work and nothing else. Core 0 holds lwIP, the W5500 driver, and everything that can block on a socket. A stalled network — unplugged cable, host not listening, ARP delay — cannot extend a control period, because the control loop's only interaction with the network is a non-blocking queue push.

### 4.3 Timing budget for one 50 ms period

`ctrl_task` wakes from `vTaskDelayUntil` on a fixed 50 ms cadence. Measured against that budget: eight INA238 register reads and four TMP117 reads total roughly 1.5 ms of bus time at 400 kHz (about 3% bus utilization); control math is well under 100 µs; the sample push and duty updates are a few tens of microseconds. Jitter is logged as the delta between scheduled and actual wake time, so the dataset carries evidence that sampling was uniform rather than an assertion that it was.

### 4.4 Sample record and queueing

Each period produces one fixed-shape sample: monotonic timestamp (`esp_timer_get_time`, 64-bit µs), sequence number, mode, four rail voltage/current/power triplets, four temperatures, fan duty command, two fan RPMs, heater duty command, computed heater power, control-law internals (error, P/I/D terms, feedforward term), fault flags, and loop jitter. The sample is pushed to a 64-deep queue; if the queue is full the sample is dropped, a counter increments, and the drop count travels in the next record. **The control loop never blocks on the network.** Acceptance criterion #3 — no dropped samples over a full profile — is verified against this counter and the sequence numbers.

### 4.5 Mode state machine

Modes are exactly the six in the CONOPS. Transitions are commanded by the host, except Failsafe, which any trip source can force from any mode and which no control law can inhibit.

- **Off/Standby** — power-on state. Heater enable low, fans idle, telemetry still streaming. Entry to any actuating mode requires a passed self-test.
- **Initialization/Self-Test** — enumerate every I²C address, verify each TMP117 reads a plausible ambient value, spin each fan and confirm tach response, confirm heater enable is low and gate driver is responsive, confirm the host link. Any failure blocks actuating modes and is reported with the specific failing check.
- **Characterization** — open loop. Operator fixes heater duty and fan duty; the BMC holds both and logs.
- **Reactive Cooling** — PID on spreader-plate temperature. VRM current is logged but not used.
- **Proactive Cooling** — feedforward on 48 V current plus feedback trim.
- **Failsafe** — latched. Exit requires an explicit host acknowledgment and a passed self-test.

Mode and gains are runtime-selectable over the command socket without reflashing, which is the WILL requirement that makes the paired comparison runs possible on one binary.

## 5. Control laws

Both laws run in the same `ctrl_task` slot, at the same 20 Hz, write to the same duty output path, and are subject to the same limits. Only the computation of the duty command differs. This is deliberate: a difference in sample rate or actuation path between the two modes would confound the comparison the project exists to produce.

### 5.1 Reactive (control group)

Discrete PID on spreader-plate temperature against a setpoint, with a fixed 50 ms step:

```
e[k]      = T_setpoint - T_plate[k]
P         = Kp * e[k]
I        += Ki * e[k] * dt          (clamped, conditional integration)
D         = -Kd * (T_plate[k] - T_plate[k-1]) / dt   (low-pass filtered)
duty[k]   = clamp(P + I + D, DUTY_MIN, DUTY_MAX)
```

Derivative acts on the measurement rather than the error, so a setpoint change does not produce a derivative kick. The integrator uses conditional integration: it stops accumulating whenever the output is saturated at either limit, which prevents the windup that would otherwise dominate a long full-load hold. `DUTY_MIN` is the fan stall floor; `DUTY_MAX` is full speed.

### 5.2 Proactive (experimental group)

A feedforward term derived from 48 V load current, summed with a feedback trim:

```
P_load[k] = V_48[k] * I_48[k]                    (from INA238 #1)
FF        = duty_map(P_load[k])                  (static map from characterization)
trim     += Ki_t * e[k] * dt   (+ optional Kp_t * e[k])
duty[k]   = clamp(FF + trim, DUTY_MIN, DUTY_MAX)
```

`duty_map` is not guessed. It is built from Characterization mode: at each heater step, the operator records the fan duty that holds the plate at setpoint in steady state, producing a small table of (load power → required duty) points that is fit and stored in NVS. The feedforward therefore encodes the plant's measured steady-state relationship, and the trim only has to correct what the map gets wrong. This is what makes the comparison defensible — the feedforward is derived from measurement, not tuned until it wins.

The trim term is intentionally weaker than the reactive PID (typically integral-dominant with little or no proportional action), because its job is steady-state correction, not transient response. If both a strong feedforward and a strong feedback term are present, the mode is not testing what it claims to test.

### 5.3 Fairness, tuning, and switching

- The same tuning method is applied to both controllers with comparable effort, and the method is recorded in the run metadata (CONOPS open item A-4 must close before data collection).
- Gains, setpoint, and the duty map are configuration, not constants in the source. Every record streamed to the host carries the active gain set, so no dataset can be orphaned from the parameters that produced it.
- Mode switches are bumpless: on entry, the new law's integrator is initialized so its first output equals the duty currently commanded. Without this, every mode change injects a transient that would appear in the data as a control artifact.
- Both modes log the same fields, including the feedforward term in Reactive mode (where it is computed but not applied), so the two runs can be overlaid directly.

## 6. Failsafe design

Failsafe is evaluated by `safety_task` at a higher priority than the control loop and from the same freshly acquired sample set. It does not consult the active control law and cannot be inhibited by it.

| Trip source | Condition | Detection |
| --- | --- | --- |
| Over-temperature | Any temperature above its limit (plate limit is the binding one) | Two consecutive samples above threshold |
| Loss of telemetry | A sensor stops responding or returns a stale/implausible value | 3 consecutive failed transactions on any device |
| I²C bus fault | NACK storm, bus stuck low, arbitration error | Driver error return plus SCL/SDA level check |
| Fan stall | Commanded above duty floor, tach reads zero | 3 consecutive 250 ms gates |
| Loop overrun | Control period misses its deadline repeatedly | Jitter watchdog in `ctrl_task` |
| Firmware hang | Task stops feeding the watchdog | FreeRTOS task watchdog, then RTC watchdog reset |

**Failsafe action, in order:** clear HEATER ENABLE (GPIO40) and zero the heater PWM; command both fans to 100%; latch the state; log the trip cause with its timestamp and the sample that caused it; report the trip to the host on the next telemetry record and on the command socket; refuse all mode-change commands until explicitly cleared.

**Latching and recovery are deliberately manual.** Clearing requires a host `CLEAR` command, and the BMC then re-enters Standby and requires a passed self-test before any actuating mode. Automatic recovery is explicitly not implemented: a fault that clears itself and re-applies heat is the failure mode most likely to hurt someone.

**Watchdogs.** The FreeRTOS task watchdog subscribes `ctrl_task` and `safety_task` with a timeout of a few control periods. If either stops feeding it, the handler drives the heater enable low before resetting. On reset, the boot path drives heater enable low as the first GPIO operation, before any peripheral or network initialization, so a reboot under load cannot leave heat applied while the stack comes up. The heater enable line is pulled down externally as well, so an unprogrammed, held-in-reset, or unpowered BMC leaves the heater off.

**Fan behavior on BMC loss.** Driving fans to full on loss of BMC control is a CONOPS MAY item and is not implemented in firmware, but the PWM idle state should be chosen so the fans do not stop when the BMC is absent. This is a hardware-side note for the Cooling owner, not committed BMC scope.

## 7. Host interface

### 7.1 Link and addressing

W5500 brought up through `esp_eth` on SPI, 10/100 full duplex, auto-negotiation. Static IP is the default (BMC 192.168.10.20/24, host 192.168.10.10), stored in NVS and overridable by command, with DHCP as a fallback if no static configuration is present. A static address removes lease timing from the list of things that can interrupt a ten-minute run. Wi-Fi uses the same sockets and the same protocol if Ethernet is unavailable.

### 7.2 Telemetry: UDP, port 9000

One datagram per sample, 20 Hz, sent from `net_tx_task`. UDP is used because a dropped sample must never stall the control loop or inflate the next period, which is exactly what TCP retransmission and head-of-line blocking would do.

**Encoding: CSV text lines (recommended).** One record per datagram, comma-separated, with a header line re-emitted every 200 records so a capture started mid-run is still self-describing. CSV is chosen over JSON and over a packed binary struct because the logs feed analysis scripts directly, the format survives being opened by a human mid-debug, and there is no schema drift between firmware and host parser. At roughly 250 bytes per record, 20 Hz is about 5 kB/s — irrelevant against a 100 Mbit link, so the compactness advantage of binary buys nothing here.

Field order: `seq, t_us, mode, v48, i48, p48, v12, i12, p12, v5, i5, p5, v3v3, i3v3, p3v3, t_inlet, t_outlet, t_plate, t_sink, fan_duty, fan1_rpm, fan2_rpm, heat_duty, heat_power_w, err, p_term, i_term, d_term, ff_term, faults, jitter_us, drops, pi_cpu_temp, pi_load`.

**Guaranteeing log continuity.** Every record carries a monotonic sequence number, so the host detects any gap. The BMC also retains the last 4096 samples in a PSRAM ring buffer (roughly the last 3.4 minutes at 20 Hz), and the host can request any missing range over the command socket with `RESEND`. Acceptance criterion #3 is therefore satisfied by construction rather than by hoping UDP behaves: the host reassembles a gapless log and reports any range it could not recover.

### 7.3 Commands: TCP, port 9001

One client at a time, newline-terminated ASCII, one command per line, one response per command. Every response is `OK <echo>` or `ERR <code> <reason>`, so an operator can drive the entire system from a terminal with no tooling — which matters for a platform whose primary purpose is instructional.

| Command | Effect | Valid in |
| --- | --- | --- |
| `PING` | Liveness, returns uptime | Any mode |
| `STATUS` | Mode, faults, active gains, link state | Any mode |
| `SELFTEST` | Run Initialization/Self-Test, return per-check results | Standby |
| `MODE <name>` | Transition to CHAR / REACTIVE / PROACTIVE / STANDBY | After passed self-test |
| `SETPOINT <degC>` | Plate temperature setpoint | Any mode |
| `GAINS <kp> <ki> <kd>` | Reactive PID gains | Standby, CHAR |
| `FFMAP <points…>` / `FFTRIM <ki> <kp>` | Feedforward map and trim gains | Standby, CHAR |
| `HEAT <pct>` | Heater duty command | CHAR, or via profile |
| `FAN <pct>` | Direct fan duty | CHAR only |
| `PROFILE START <name>` / `PROFILE STOP` | Run or abort a stored load profile | Actuating modes |
| `RUN START <id> <epoch_us>` / `RUN STOP` | Mark run boundaries, sync host time | Any mode |
| `RESEND <seq_a> <seq_b>` | Replay buffered samples | Any mode |
| `CLEAR` | Acknowledge and clear a latched Failsafe | Failsafe only |

A command that is invalid for the current mode is rejected with a reason rather than silently ignored. **No command can inhibit a trip, clear a fault without acknowledgment, or raise a temperature limit above its compiled maximum.** The host is an operator interface, not a safety authority.

### 7.4 Timestamps and run metadata

Records carry `esp_timer` microseconds since boot, which is monotonic and immune to any host clock adjustment. `RUN START` supplies the host's wall-clock epoch, which the BMC stores as a single offset and echoes in the run header, so the host can convert to absolute time in post-processing without ever altering the monotonic timebase mid-run. The run header also records firmware git hash, active gains, setpoint, sensor addresses, and the profile name, so a dataset is reproducible from the log alone.

## 8. Bring-up, verification, and open items

### 8.1 Bring-up order

Each step is independently demonstrable, and nothing that applies heat happens before the sensing and failsafe paths are proven.

1. Board bring-up: ESP-IDF toolchain, blink, UART console, `bmc_board` pin map. Early work can run on the ESP32-WROOM-32E kit; only the pin map and target change.
2. Ethernet: W5500 link up, static IP, UDP echo to the host, TCP command socket with `PING` and `STATUS` only.
3. I²C bring-up: bus scan, one TMP117 read, one INA238 read against a bench reference for calibration confidence.
4. Full telemetry chain: all eight devices at 20 Hz, records streaming, jitter and drop counters visible.
5. Fans: PWM sweep, duty floor characterization, tach across the range, stall detection proven by unplugging a fan.
6. Failsafe path, before any heat: force each trip source and confirm heater enable goes low, fans go to 100%, and the state latches.
7. Heater, low power first: gate driver verification, then a small duty step with the plate sensor watched live.
8. Characterization runs, `duty_map` construction, then controller tuning and the paired Reactive/Proactive datasets.

### 8.2 Mapping to CONOPS acceptance criteria

| # | Criterion | How the BMC satisfies it |
| --- | --- | --- |
| 3 | Enumerates all telemetry sources; no dropped samples | Self-test address enumeration; sequence numbers plus `RESEND` from the PSRAM ring buffer |
| 4 | Both modes runtime-selectable, same profile | `MODE` command, one binary, no reflash; shared actuation path |
| 5 | Paired datasets under matched conditions | Identical record schema in both modes, gains and profile in the run header |
| 6 | Over-temperature and fault failsafe assert and latch | `safety_task` trip table, manual `CLEAR`, self-test required to resume |

### 8.3 Open items this design does not close

- **A-2, sampling rate and scheduling** — this document proposes 20 Hz with the timing budget above, but it is a proposal until loop jitter is measured on the real bus with all eight devices attached.
- **A-3, sensor BOM and placement** — the TMP117's four-address limit caps the bus at four temperature sensors. If a fifth location is needed, that decision changes the part or adds a bus.
- **A-4, proactive control law form and tuning** — the feedforward map is specified in method here, not in value. It must be fixed and documented before data collection.
- Heater gate driver part selection, and whether per-element current sensing (a MAY item) is worth a fifth INA238.
- Whether the Ethernet-primary decision in Section 1 is accepted by the sponsor as an early MAY implementation or should be formally moved to WILL in the Change Record.
