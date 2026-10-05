# BMC Telemetry Dashboard

Live browser dashboard for the BMC. Python standard library only (3.8+), no installs.

```
 ┌────────────── ESP32 BMC ──────────────┐            ┌──────────── host computer ────────────┐
 │ core 1: sensor_task / fan_task        │            │ server.py                              │
 │    └─ telemetry_publish()  (non-block)│            │   UDP :9000  ─► parse, gap-detect,     │      ┌─────────┐
 │ core 0: net_tx_task ── bmc_net ───────┼─ UDP/CSV ─►│                 history, CSV log       ├─SSE─►│ browser │
 │         (SoftAP now, Ethernet later)  │◄─ HELLO ───┤   HTTP :8080 ─► static/ + /events      │      └─────────┘
 └───────────────────────────────────────┘            └────────────────────────────────────────┘
```

## Run it

```powershell
cd dashboard
python server.py                  # real BMC on its SoftAP (BMC = 192.168.4.1)
# open http://localhost:8080
```

No hardware? Run a fake BMC that speaks the same protocol:

```powershell
python server.py --bmc-ip 127.0.0.1 --bmc-port 9100
python simulate_bmc.py --port 9100 --loss 0.03     # --loss simulates dropped samples
```

Other devices on the BMC's Wi-Fi (phone, second laptop) can open `http://<host-ip>:8080`
(the server prints the addresses). Use `--http-host 127.0.0.1` to keep it local-only.

Windows firewall must allow inbound **UDP 9000** and **TCP 8080** for Python (admin PowerShell):

```powershell
New-NetFirewallRule -DisplayName "BMC dashboard UDP" -Direction Inbound -Protocol UDP -LocalPort 9000 -Action Allow
New-NetFirewallRule -DisplayName "BMC dashboard HTTP" -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow
```

Every record is appended to `logs/telemetry_<timestamp>.csv` (git-ignored). `--no-log` disables it.

## Wire protocol (identical over Wi-Fi and Ethernet)

**Subscribe.** The server sends `HELLO\n` (UDP) to the BMC at `--bmc-ip:--bmc-port` every 2 s from its
telemetry port. The BMC remembers each sender (up to 4) and streams to it, forgetting a sender after
10 s without a HELLO. Nothing in the BMC needs to know the host's IP in advance, and several viewers
can subscribe. Switching to Ethernet only changes `--bmc-ip` (e.g. `192.168.10.20`).

**Telemetry.** One CSV record per datagram, UDP port 9000, 10 Hz:

```
bmc_id,seq,t_us,temp_c,power_w,fan_duty_pct,fan_en,fan_rpm,drops,flags
bmc0,1042,104200311,41.2031,9.874,26.4,1,1005,0,0
```

| Field | Meaning |
|---|---|
| `bmc_id` | Board identifier (lets one dashboard tell several BMCs apart) |
| `seq` | Monotonic per-boot counter. Gaps = datagrams lost in transit; going backwards = BMC reboot |
| `t_us` | `esp_timer` microseconds since boot (monotonic; the time axis of every chart) |
| `temp_c`, `power_w` | Sensor readings. **Empty field = reading unavailable** (chart shows a break) |
| `fan_duty_pct`, `fan_en`, `fan_rpm` | Commanded duty, interlock state (0/1), measured speed |
| `drops` | Records the BMC itself discarded (queue full or link down) |
| `flags` | Reserved bit field (faults, mode) |

The BMC re-emits a header line (`bmc_id,seq,t_us,…`) every 200 records and after each HELLO from a new
subscriber. The server maps fields **by header name**, so adding a column in firmware never breaks the
host. The dashboard only draws the columns it knows and logs all of them.

## Firmware (implemented, builds on ESP-IDF 6.1)

| Component | Role |
|---|---|
| `bmc_telemetry` | `telemetry_sample_t` + queue. `telemetry_publish()` is a zero-timeout push (core 1); a full queue only bumps the drop counter. |
| `bmc_net` | The only file that knows the link: `bmc_net_ap.c` is the Wi-Fi SoftAP (`BMC-Telemetry`, BMC = 192.168.4.1, WPA2). The ESP32-S3-ETH port replaces it with an `esp_eth`/W5500 version of `bmc_net_init()` / `bmc_net_ready()`. |
| `bmc_stream` | Core-0 `net_tx` task: BSD sockets only. Answers HELLO, keeps up to 4 subscribers (10 s timeout), sends CSV, header every 200 records and to each new subscriber. |
| `main.c` | `sensor_task` now runs at 10 Hz on `vTaskDelayUntil`, publishes one sample per period (OLED redraw and log lines every 10th). `hold_task` is gone; core 0 runs Wi-Fi + `net_tx`. |
| `tmp117` | `tmp117_init()` now writes the config register (continuous, no averaging, ~15.5 ms cycle). The power-on default produced a fresh value only once per second. |

Settings: `idf.py menuconfig` -> *BMC network* (SSID, password, channel, max clients) and
*BMC telemetry stream* (UDP port, `bmc_id`). `drops` counts samples lost to a full queue or a failed
send; samples produced while nobody is subscribed are not counted.

**Password note:** `sdkconfig` is tracked in git, so the configured password is committed with it.
The default (`bmctelemetry`) is fine for the bench; change it before sharing the repo widely.

**Control isolation check (do on hardware):** with the host unplugged or Wi-Fi off, fan-loop timing and
the OLED must not change. Core 1 only ever does a non-blocking queue push.

## Files

| Path | Purpose |
|---|---|
| `server.py` | UDP receiver + HELLO sender, history, CSV logger, HTTP/SSE server |
| `simulate_bmc.py` | Fake BMC for testing without hardware |
| `static/` | The dashboard page (`index.html`, `app.js`, `style.css`), no external dependencies, works offline |
