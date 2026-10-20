# 08 — Information Flow: How Everything Talks

Describes the data paths in the firmware on branch `feature/bmc-state-machine` **as implemented now**, and the expansion points already built in. Companion to `01_architecture.md` (the design intent) and `07_implementation_status.md` (what is verified). Everything below was read from the code; hardware behaviour is still to be proven on the bench.

Contents: 1 The picture · 2 The two cores · 3 Every crossing between tasks · 4 Flow A: sensing to actuating · 5 Flow B: telemetry out · 6 Flow C: a command round trip · 7 Flow D: failsafe to operator · 8 The SoftAP link · 9 What keeps control independent of the network · 10 Who owns what · 11 What happens when something breaks · 12 Built-in expansion points · 13 Where to find each piece in the code

---

## 1. The picture

```
                       LAPTOP (same Wi-Fi as the BMC)                                     ESP32 BMC (WROOM-32E now, S3 later)
  ┌──────────────────────────────────────────────────────┐        ┌───────────────────────────────────────────────────────────────────────┐
  │ browser ◄──HTTP/SSE :8090──► server.py               │        │  CORE 0 (network)                CORE 1 (control, never touches sockets)│
  │  (dashboard UI)              │  UDP :9000            │ Wi-Fi  │  ┌───────────┐                   ┌──────────────┐  prio 10, every 50 ms │
  │                              │  ├─ telemetry in      │◄══════►│  │ Wi-Fi AP  │                   │  supervisor  │──► heater + fans      │
  │                              │  ├─ events in         │ SoftAP │  │ + lwIP    │                   │  (safety +   │    (only writer)      │
  │                              │  ├─ replies in        │ BMC-   │  └─────┬─────┘                   │  fan loops + │                       │
  │                              │  ├─ HELLO out (2 s)   │ Telem. │  ┌─────▼─────┐   queues          │  brain_step) │                       │
  │                              │  ├─ commands out      │ 192.   │  │ net_tx    │◄══════════════════│              │                       │
  │                              │  └─ heater keepalive  │ 168.   │  │ task      │══════════════════►│              │                       │
  │  logs/telemetry_*.csv        │     out (250 ms)      │ 4.1    │  │ prio 5    │  (see section 3)  └──────▲───────┘                       │
  │  logs/events_*.csv           │                       │        │  └───────────┘                          │ snapshot               │
  └──────────────────────────────────────────────────────┘        │                                  ┌──────┴───────┐  prio 8, every 100 ms │
                                                                  │                                  │   sensors    │◄── I2C: TMP117, INA238│
                                                                  │                                  └──────────────┘                       │
                                                                  │                                  aux (prio 2): OLED, fan-link peer      │
                                                                  └───────────────────────────────────────────────────────────────────────┘
```

Three rules explain the whole design:

1. **Core 1 decides and acts; core 0 only moves bytes.** Core 1 never calls a socket; core 0 never touches the heater or fans.
2. **Everything that crosses between the cores is a non-blocking queue.** If a queue is full the data is dropped and counted; nobody waits. A dead network therefore cannot slow a control cycle.
3. **One component writes the outputs.** Heater and fan pins are written only by `act_apply()`, called only from the supervisor task, and only after the safety verdict has been applied.

---

## 2. The two cores

| | Core 1 — control | Core 0 — network |
|---|---|---|
| Tasks | `supervisor` (prio 10, 50 ms), `sensors` (prio 8, 100 ms), `aux` (prio 2, 100 ms) | `net_tx` (prio 5, wakes at least every 50 ms), plus the ESP-IDF Wi-Fi driver tasks (ESP-IDF's default placement; the firmware only pins `net_tx`) |
| Touches | I2C bus, tach counters, heater PWM and enable pin, fan PWM, permit/interlock pins, OLED | UDP socket, Wi-Fi |
| Never touches | sockets (replies and events go through queues) | heater, fans, I2C, any decision |
| Blocking allowed? | No: no network calls, no waits on queues (zero-timeout pushes) | Yes, but bounded; the socket is non-blocking too |
| Watchdog | `supervisor` and `sensors` each must run within 1 s or the chip resets (panic) | not watched, so Wi-Fi load cannot reset the controller |
| Only I/O besides pins | `ESP_LOG` to the serial console (events are logged after the outputs are already safe) | |

Why this split: Wi-Fi/lwIP work is bursty and can stall for tens of milliseconds. Putting everything that can block on core 0, and making the only connection between the cores a set of drop-on-full queues, means a stalled or absent network cannot extend a control period (this is the "control isolation check" in `07`, T18).

### The cycle on core 1 (`supervisor`, every 50 ms, in this order)

```
 1  feed the watchdog
 2  take up to 4 queued commands (from the net task), parse, execute via brain_command(), queue the replies
 3  copy the latest sensor snapshot (plate, load current, failure counters, "stuck bus" flag, age)
 4  read pins: fan interlock, heater permit
 5  read both tach counters -> RPM; update the two stall detectors
 6  brain_step()  ──►  safety state machine → self-test → control law (once per fresh 10 Hz sample)
                       → profile → heater arbitration                     (all pure logic, no hardware)
 7  fan layer: failsafe = both fans 100 % open loop; self-test = test duties; else RPM-match both fans
 8  act_apply(): the ONLY place heater and fan outputs are written
 9  publish app_status (mode, fault, flags, duties, RPMs) for the sensors/aux tasks
10  drain new events -> queue for the net task (and log to serial)
11  sleep until the next 50 ms tick
```

### The cycle on core 1 (`sensors`, every 100 ms)

```
 poll plate TMP117, (inlet TMP117 if built in), INA238  →  count consecutive failures per device
 after any failure: read SDA/SCL, if stuck low or the whole bus is dead → reset the I2C controller (rate limited)
 a device that answers again after failing is re-configured (it may have been power-cycled)
 publish the sensor snapshot (spinlock)  →  build one telemetry record from snapshot + app_status
 telemetry_publish(): zero-timeout push into the telemetry queue
```

`aux` does the slow optional I2C work (OLED redraw at 1 Hz, the fan-link peer) so a missing peripheral can never delay a sensor read.

### The loop on core 0 (`net_tx`)

```
 wait up to 50 ms for a telemetry record
 read any incoming UDP: HELLO → add/refresh a subscriber;  anything else from a subscriber → command callback
 expire subscribers silent for 10 s
 send queued command replies
 send queued events (each three times, 100 ms apart)
 if a record arrived: format it as CSV and send it to every subscriber (header first / every 200 records)
```

---

## 3. Every crossing between tasks

These are the **only** ways data moves between tasks. Everything else is private to its task.

| # | Name | From → To | Mechanism | Size | When full or slow |
|---|---|---|---|---|---|
| 1 | Sensor snapshot | sensors → supervisor, aux | shared struct under a short spinlock, copied out | ~80 B | the reader just sees the previous one; its **age** is measured and 500 ms of silence trips failsafe |
| 2 | App status | supervisor → sensors, aux | shared struct under a spinlock | ~150 B | stale values in telemetry for one cycle at worst |
| 3 | Telemetry queue | sensors (core 1) → net_tx (core 0) | FreeRTOS queue, **zero-timeout push** | 32 records (3.2 s) | record dropped, `drops` counter incremented, visible on the dashboard |
| 4 | Command queue | net_tx (core 0) → supervisor (core 1) | FreeRTOS queue, zero-timeout push | 8 lines of ≤ 64 chars | command lost; the host retries (commands are idempotent) |
| 5 | Reply queue | supervisor → net_tx | FreeRTOS queue, zero-timeout push | 6 replies of ≤ 512 B | reply lost; the host retries the command |
| 6 | Event queue | supervisor → net_tx | FreeRTOS queue, zero-timeout push | 8 lines | event not sent live; it is still in the BMC's event ring (last 16) and in every telemetry record's fault fields, and can be replayed with `EVENTS` |
| 7 | Event ring | brain (inside supervisor) | 16-entry ring with a "sent" cursor | 16 events | oldest overwritten; a slow consumer skips ahead |
| 8 | I2C bus | sensors, aux → devices | ESP-IDF driver bus lock | — | a transfer waits for the other task (the OLED flush can delay a read by a few ms) |

The telemetry path has the strongest guarantee: the producer never waits, so network trouble costs data on the wire, never timing in the control loop.

---

## 4. Flow A — sensing to actuating (the control path)

```
 TMP117 (plate) ─┐                                                      ┌─► heater PWM  (GPIO23, 40 kHz, filtered analog demand)
 INA238 (48 V) ──┼─ sensors task ──snapshot──► supervisor ──► brain ──► ├─► heater enable (GPIO27, mirrored to the LED)
 (inlet, VRM) ───┘   count failures        │                  │        └─► fan 1 / fan 2 PWM (25 kHz, GPIO18 / GPIO25)
                                            │                  │
 tach 1 / tach 2 (PCNT) ───────────────────►│                  │  safety verdict is applied LAST:
 fan interlock pin, heater permit pin ─────►│                  │  proposals can never override it
```

Inside `brain_step()` the order matters (this is what makes the safety layer independent of the control law):

```
 1 safety_step      evaluates the fault table, runs the mode state machine, latch, cooldown timer
 2 self-test        if mode == SELFTEST: sequence the checks, command the fan test duties
 3 control law      once per fresh sensor sample: REACTIVE = PID; PROACTIVE = feedforward + PID trim;
                    CHAR = manual fan (or the temperature curve); STANDBY/other = the temperature curve
 4 profile          heater level from the running timeline, or the manual HEATER lease (CHAR only)
 5 arbitration      heater allowed only if: mode permits heat, not failsafe, permit pin closed,
                    fan interlock closed, plate temperature readable. Then clamped to the bring-up cap.
                    Failsafe forces: heater 0, both fans 100 % open loop.
 6 status           builds the telemetry/flags snapshot
```

Timing: sensors sample every 100 ms; a new reading is acted on by the next supervisor cycle (≤ 50 ms later). Fault-to-safe latencies (design values from `02`): over-temperature ≤ 50 ms after the reading; sensor or bus loss ≈ 300–400 ms (three failed reads); fan stall ≈ 250 ms tach window plus one cycle; a hung sensor task 500 ms; a hung supervisor 1 s (watchdog reset).

Data the control path produces for analysis (and how it reaches the log): `ctl_p`, `ctl_i`, `ctl_ff`, `fan_target_rpm`, `heater_cmd_pct` vs `heater_pct`, all in every telemetry record.

---

## 5. Flow B — telemetry out (BMC → laptop → browser)

```
 sensors task (core 1, every 100 ms)
   └ builds telemetry_sample_t  (plate, current, power, heater, fans, mode, fault, flags ...)
   └ telemetry_publish()  ── stamps seq + t_us ── zero-timeout push ──►  [telemetry queue, 32]
                                                                              │
 net_tx task (core 0) ◄───────────────────────────────────────────────────────┘
   └ bmc_fmt_record()  →  one CSV line (31 columns; empty field = not valid, never a fake zero)
   └ sendto() every subscriber  (UDP :9000, header line first and every 200 records)
                                   │  Wi-Fi SoftAP
 server.py (laptop)  ◄─────────────┘
   ├ parse by header name · detect gaps from seq · detect reboots (seq goes backwards)
   ├ continuity statistics (longest gap, out-of-order stamps, records missing a field)
   ├ keep 10 minutes of history (6 000 samples) · append to logs/telemetry_<time>.csv
   └ push to every browser as a Server-Sent Event "sample"  (plus a 1 Hz "status" event)
 browser: charts, state banner, table; also fetches /api/history on load
```

Key properties:

- **Timestamps come from the BMC** (`t_us`, stamped at publish), so network jitter never appears as a timing error in the data.
- **Loss is observable at three levels:** `seq` gaps (lost in the air), `drops` (discarded inside the BMC), and the host's own continuity report.
- **No local storage on the BMC.** The host is the record, so a storage fault cannot cost a run.
- **Adding a column cannot break the host:** the server maps fields by the header the BMC sends. Unit tests check that the record and the header always have the same column count.

---

## 6. Flow C — a command round trip (browser → BMC → browser)

Example: the operator clicks *Run self-test*.

```
 browser          server.py (laptop)             net_tx (core 0)        supervisor (core 1)
   │ POST /api/cmd {"cmd":"SELFTEST"}
   ├────────────────►│ whitelist the verb, ≤ 60 printable chars, allowed commands only
   │                 │ assign token #17
   │                 ├─ UDP "#17 SELFTEST\n" ───────────►│ is the sender a current subscriber? (else ignored)
   │                 │                                    │ copy into the command queue (non-blocking)
   │                 │                                    ├──────────────────────────►│ next 50 ms cycle:
   │                 │                                    │                           │  parse → brain_command()
   │                 │                                    │                           │  safety decides legal/illegal
   │                 │                                    │◄──── reply queue ─────────┤  "#17 OK SELFTEST started"
   │                 │◄──────── UDP "#17 OK SELFTEST started" ──┤
   │                 │ match token, release the waiting request
   │◄────────────────┤ {"ok":true,"reply":"OK SELFTEST started"}
   │ (the state change itself shows up in the next telemetry records: mode=SELFTEST)
```

- **Latency:** command executed within one supervisor cycle (≤ 50 ms) of arriving; the server waits 0.7 s for the reply and retries once. Because every command is idempotent, a retry after a lost reply is harmless.
- **Where legality is decided:** only in the BMC (`bmc_safety.c` and `brain_command()`). The dashboard greys buttons out from telemetry as a convenience, but it cannot grant anything the BMC refuses (`ERR MODE no_selftest`, `ERR ACK not_safe`, `ERR HEATER mode`, ...).
- **Who may command:** only a host that has sent HELLO in the last 10 s (the subscriber table). Over-long lines (> 64 chars) are rejected, not truncated into valid commands.
- **Every command has a documented effect in** `03_interfaces.md`; the parser is portable C with unit tests, so adding a command is a change in two places (`bmc_cmd.c`, `brain_command()`).

**The manual heater is a special case.** The dashboard switch becomes `HEATER <0|1> <pct>` datagrams every 250 ms from the server's own timer, with a 3 s lease on the server side and a 1 s lease on the BMC side. A closed tab, a dead server or a lost link all end up as "no keep-alive", so the heater request lapses. `HEATER 1` is only accepted in Characterization mode; `HEATER 0` is accepted silently in any mode so the constant keep-alive causes no reply traffic.

---

## 7. Flow D — from a fault to the operator

```
 fault condition appears (e.g. fan stalls)
   │
   ▼ supervisor cycle ≤ 50 ms later
 safety_step: condition armed? → mode = FAILSAFE, latch first cause, selftest_ok = false
   │
   ├► arbitration this same cycle:  heater = 0, enable low;  fan_full → both fans 100 % duty
   ├► act_apply() writes the pins            ◄── this happens BEFORE anything below
   │
   ├► event ring: FAILSAFE_ENTER, cause, plate temperature, counters, timestamp
   ├► drain → event queue → net_tx → UDP "EVENT,<id>,<t_us>,,FAILSAFE_ENTER,FAN_STALL,..." sent 3× (100 ms apart)
   ├► serial log (ESP_LOGE), OLED shows FAILSAFE and the cause
   └► every telemetry record from now on carries mode=FAILSAFE, fault_code, fault_mask, flag bits
          (the backstop if the event datagrams are lost)
 server.py: de-duplicate on (boot, id) → logs/events_*.csv → SSE "bmcevent" → red banner + event list
 recovery:  plate < 60 °C for 5 s → SAFE_TO_HANDLE event/flag → operator clicks Acknowledge
            → ACK command → STANDBY, self-test must pass again before any heat
```

The operator is told by the live event, by the state banner (driven by telemetry, so it is correct even if the event was missed), and by the serial console/OLED. A host that connects late or restarts asks for the last 16 events with `EVENTS` automatically when it sees a new boot.

---

## 8. The SoftAP link

### How the link is formed

The BMC is its own Wi-Fi access point; there is no router in the system.

| Item | Value (menuconfig → *BMC network*) |
|---|---|
| Network name | `BMC-Telemetry` |
| Security | WPA2-PSK, password `bmctelemetry` (change for anything beyond the bench; `sdkconfig` is tracked in git) |
| Band | 2.4 GHz only (ESP32 hardware), channel 1 |
| BMC address | `192.168.4.1/24`, with a built-in DHCP server that hands the laptop `192.168.4.2` and so on |
| Max clients | 4 simultaneous stations |
| Internet | none; a laptop on this network loses internet unless it has a second adapter |

`bmc_net_init()` (`bmc_net_ap.c`) starts the AP and sets a ready flag from the Wi-Fi events. `bmc_net_ready()` is the only thing the rest of the firmware knows about the link. If the AP fails to start, the BMC logs it and keeps controlling; the network is never required for safety.

### How the laptop and the BMC find each other

The BMC does not need to know the laptop's address. The **laptop announces itself**:

```
 server.py ──► UDP "HELLO" from local port 9000 to 192.168.4.1:9000, every 2 s
 BMC       ──► stores that sender (IP + port) as a subscriber; refreshes it on every HELLO
           ──► streams telemetry and events to all subscribers; accepts commands only from them
           ──► forgets a subscriber after 10 s without a HELLO  (up to 4 subscribers)
```

Consequences:

- Several viewers can watch at once (up to 4), e.g. two laptops, with only one needing control in practice.
- Closing the dashboard stops the stream within 10 s without any BMC-side configuration.
- The same mechanism works unchanged over Ethernet; only the BMC's IP changes (`--bmc-ip`).
- UDP carries everything on a single port (9000, both directions): telemetry and events out, commands and replies both ways.

### What lives where

| | On the BMC | On the laptop |
|---|---|---|
| UDP :9000 | the stream/command socket | the receive socket (same socket also sends HELLO, commands and the heater keep-alive) |
| HTTP/SSE :8090 | **nothing** — the BMC has no web server | `server.py` serves the page and the live stream to browsers |
| Storage | none | CSV telemetry and event logs |

A phone or second laptop can also join the BMC's Wi-Fi and open `http://<laptop-ip>:8090` (the server prints the address), but it talks to the laptop, not to the BMC.

### Security posture (today)

Lab instrument, WPA2 on the radio, **no authentication on the commands**. Anyone on the BMC's Wi-Fi who sends HELLO becomes a subscriber and can command it; anyone who can open the dashboard page can command it through the laptop. `python server.py --no-control` makes the page view-only. Nothing a remote sender can do bypasses the BMC's own rules (self-test first, permit pin, interlock, failsafe); the bench supply remains the real emergency stop. Authentication is listed as an expansion point below.

### When the link misbehaves

UDP is lossy by design here; every layer tolerates it (see section 11). The BMC never waits for the network, so Wi-Fi loss changes only what the operator can see.

---

## 9. What keeps control independent of the network

| Mechanism | Where |
|---|---|
| Separate cores; control tasks never call a socket | `main.c`, `supervisor.c`, `bmc_stream.c` |
| All crossings are zero-timeout queues; full = drop and count | section 3 |
| Replies and events are queued, then sent by the net task | `bmc_stream_reply`, `bmc_stream_post_event` |
| The manual heater needs a live keep-alive; silence = off | `brain_command` lease, 1 s |
| Profiles run on the BMC from its own clock | `bmc_profile.c`; a dropped link does not change a run |
| Safety verdict applied after every proposal; single writer to the outputs | `bmc_brain.c`, `actuators.c` |
| Watchdog on the two control tasks (1 s), idle tasks unwatched | `main.c` |
| Network failure at boot is logged, not fatal | `main.c` |

---

## 10. Who owns what

| Data | Owner (only writer) | Readers |
|---|---|---|
| Raw sensor readings and failure counters | `sensors` task | supervisor, aux (through the snapshot) |
| Mode, fault latch, self-test result, control state, profile, setpoint | `brain_t` inside the supervisor task | nobody else; others see `app_status` |
| Heater and fan outputs | `act_apply()` (supervisor task only; a second caller is refused and logged) | — |
| Event ring | `brain_t` | drained by the supervisor to the event queue |
| Subscriber table | `net_tx` task | — |
| Telemetry sequence and drop counters | `telemetry_publish` / `telemetry_count_drop` (atomics) | net_tx, dashboard |
| Logs and history | `server.py` | browser |

All decisions live in `bmc_core`, which has no hardware, no RTOS and no clock of its own (time is passed in). That is why the safety state machine can be tested exhaustively on a PC.

---

## 11. What happens when something breaks

| Event | Effect on control | What the operator sees |
|---|---|---|
| Wi-Fi drops or the laptop sleeps | none; fans and closed loop continue; a running profile continues; the manual heater lapses after 1 s | stream stops; dashboard shows "No data" |
| Dashboard tab closed | manual heater lapses (≤ 3 s server / 1 s BMC); everything else continues | — |
| Telemetry queue full (net task behind) | none; record dropped | `Dropped by BMC` counter rises |
| Datagram lost in the air | none | `Lost in transit` rises (from `seq` gaps) |
| Command or reply lost | none | the dashboard retries once, then shows "no reply"; resend is safe |
| Event datagrams lost | none | latched fault is still in every telemetry record; `EVENTS` replays the ring |
| Server restarts | none | new subscription within 2 s; replays events if the BMC rebooted |
| BMC reboots (power, brownout, watchdog) | boots into STANDBY, heater off, self-test required | `restarts` counter, `RESET` event with the reason |
| Sensor or bus failure while heating | failsafe within ≈ 0.4 s | red banner, event with cause |

---

## 12. Built-in expansion points

What already exists in the code to receive each future change, and what is still missing.

| Future change | What is already in place | What remains |
|---|---|---|
| **Ethernet (W5500) instead of / alongside the SoftAP** | `bmc_net.h` is the only network interface the rest of the firmware sees (`bmc_net_init`, `bmc_net_ready`); streaming uses plain BSD sockets; the dashboard needs only `--bmc-ip` | write `bmc_net_eth.c` with the same two functions; pick a static IP |
| **ESP32-S3 port** | every pin number is in `bmc_board.c` only; actuator and device layers are separate components | change target, set pins, re-run the safety bench tests (`07` section 5) |
| **Raspberry Pi 5 interface** (deferred) | I2C bus and an address are reserved in the ICD; `aux`/`sensors` already show how a bus peripheral is polled; command and event formats are text and extensible | decision on the link (I2C peripheral vs other), driver, telemetry fields, a self-test check |
| **VRM rail monitor (3.3 / 5 / 12 V)** | telemetry columns `v33_v v5_v v12_v` exist and are empty; `rails_valid` flag in the sample; the dashboard ignores them until filled | the device driver and a poll in the `sensors` task; add to the self-test and required-device list |
| **Inlet (ambient) sensor** | Kconfig option `BMC_INLET_SENSOR` (address configurable); telemetry `inlet_c`; the profile-start check uses it | enable and test; add to the fault table if it should be "required" |
| **Real heater gate driver (PWM + MOSFET)** | `act_apply()` is the single choke point; `heater_enable` line and the `HEATER_MAX_PCT_BRINGUP` cap exist; the INA238 current path is read | the gate PWM driver, PWM frequency tied to INA238 averaging, staged bring-up (`02` section 6) |
| **More than two fans** | `BMC_FAN_COUNT`, per-fan loops, stall detectors and telemetry columns are array-based in the core | board pins, LEDC/PCNT instances, extra telemetry columns |
| **More control strategies / tuning** | pluggable feedforward table (4 variants, `FF`, `FFPARAM`); PID gains settable by command; run parameters written into every `RUN_START` event | plant identification, tuned values, any new variant |
| **Uploading arbitrary load profiles** | `PROFILE CLEAR/ADD/PRESET/START/STOP` already accept steps (16 max) | a host-side profile editor |
| **New telemetry fields** | header-name parsing in the server; add a column in `bmc_fmt.c` and the sample struct; unit test checks header/record agreement | dashboard chart for it if wanted |
| **New commands / events** | one enum entry and parser branch in `bmc_cmd.c`, one `case` in `brain_command()`, one event type in `bmc_events.c` | host UI if wanted |
| **Persisting the failsafe latch across a power cycle** | reset reason is already read and logged at boot | NVS write on failsafe entry and a check at boot |
| **Authentication / stronger isolation** | commands are only accepted from subscribers; `--no-control` exists | a shared secret or token on commands; optionally a TCP command socket instead of UDP (the design notes in `03` allow either) |
| **Several BMCs or sleds on one dashboard** | `bmc_id` in every record; the server tracks per-BMC sequence and continuity | the UI currently shows the first BMC only |
| **Moving the dashboard off the laptop** | the BMC has no web server and does not need one; the server is stdlib Python | run `server.py` on any machine on the same network |

---

## 13. Where to find each piece

| Flow / topic | Files |
|---|---|
| Startup order | `main/main.c` |
| Supervisor cycle, fan loops, command intake, event draining | `components/bmc_app/supervisor.c` |
| Sensor polling, failure counting, telemetry assembly | `components/bmc_app/sensors.c` |
| The only writer of outputs | `components/bmc_app/actuators.c` |
| OLED and fan-link peer | `components/bmc_app/bmc_aux.c` |
| All decisions (state machine, arbitration, commands) | `components/bmc_core/bmc_safety.c`, `bmc_brain.c`, `bmc_selftest.c`, `bmc_profile.c`, `bmc_pid.c`, `bmc_fanrpm.c`, `bmc_ff.c`, `bmc_stall.c` |
| Wire formats | `components/bmc_core/bmc_cmd.c`, `bmc_fmt.c`, `bmc_events.c` |
| Network task, subscribers, queues | `components/bmc_stream/bmc_stream.c` |
| SoftAP | `components/bmc_net/bmc_net_ap.c`, `Kconfig.projbuild` |
| Telemetry queue | `components/bmc_telemetry/bmc_telemetry.c` |
| Pins | `components/bmc_board/bmc_board.c` |
| Laptop side | `dashboard/server.py`, `dashboard/static/`, `dashboard/simulate_bmc.py` |
| Tests of the logic | `firmware/BMC_firm/test_host/`, `dashboard/test_integration.py` |
