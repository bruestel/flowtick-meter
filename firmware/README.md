# flowtick-meter firmware

ESP-IDF firmware for the flowtick-meter reading head: an ESP32-C6 samples a
reflective sensor over the scanning disc of a water meter, counts the edges
and publishes reading, flow and status over a web interface and MQTT.

Networking, configuration, MQTT, the web interface, OTA with rollback,
watchdog, coredump and the BOOT-button recovery are infrastructure taken from
the author's bsh-dbus-idf project; the meter, the edge log and the API on top
are this project's own.

This file is the developer view. For users:

* [../docs/firmware.md](../docs/firmware.md): web interface, MQTT, LED, edge log
* [../docs/tuning.md](../docs/tuning.md): thresholds, edge filter, volume per edge
* [../docs/hardware.md](../docs/hardware.md): board, sensor circuit, wiring

## Building

```bash
pio run -e supermini-c6            # build
pio run -e supermini-c6 -t upload  # flash
pio device monitor
```

Flash size is pinned to 4 MB (`board_upload.flash_size` /
`board_build.flash_size`); without it the upload writes an 8 MB header and the
bootloader asserts. Flash directly, not behind a chain of USB hubs.

The firmware version (web UI, MQTT status, Home Assistant) comes from
`git describe --tags --always --dirty` in a git checkout, and from
`version.txt` otherwise (e.g. a source archive). Bump it with every release.

The project name in the image (`project(flowtick-meter)` in `CMakeLists.txt`)
is checked on OTA: an image of another project is refused.
`same_project()` in `app_web` also accepts the former name `water-meter`, so
devices flashed before the rename update over the air.

Afterwards, OTA from the web UI or:

```bash
curl -X POST -H 'Content-Type: application/octet-stream' \
     --data-binary @.pio/build/supermini-c6/firmware.bin http://<ip>/api/v1/ota
```

ESP-IDF 6.0.1 via `espressif32@7.0.1`. On the first build the component
manager fetches cJSON, esp-mqtt and mDNS, pinned by `dependencies.lock`.

Configuration is **not** compiled in: on first boot the device opens its own
WiFi network and serves a captive portal. Enter WiFi and MQTT there; after
that everything goes through the web interface or OTA.

## Components

| Component | Purpose |
|---|---|
| `app_net` | WiFi station with AP fallback, captive portal, scan and test-before-store connect, transmit power, SNTP (configurable server + built-in fallback, timezone) |
| `app_config` | settings in NVS, PBKDF2 password (stored as one atomic entry) |
| `app_mqtt` | MQTT / MQTTS (built-in CA bundle, own CA or unverified), HA auto-discovery, LWT |
| `app_web` | HTTP, auth, **OTA with rollback**, WebSocket, the REST API |
| `meter` | ADC, edge detection, register, leak watch, NVS |
| `edgelog` | append-only ring of edges in the `edgelog` flash partition |
| `src/` | `main.cpp` wiring, board pins, status LED, diagnostics, BOOT-button recovery, the trial period of a new image (`probation`) |

WiFi power save is off while the scope runs. The default device name is
`flowtick-xxxxxx` from the last three MAC bytes; the Home Assistant device
identity stays `water-meter-xxxxxx` (`appcfg::device_id()`) so devices from
before the rename keep their entities.

## Edge detector

Hysteresis alone is not enough. On top of `thr_hi_mv`/`thr_lo_mv`
(defaults 2200/1200 mV, tuned for the first meter; see
[../docs/tuning.md](../docs/tuning.md)):

* **dwell** (`dwell_ms`, default 80): a crossing counts only once the signal
  has stayed beyond the threshold that long; shorter ones count as `rejected`.
* **minimum gap** (`min_edge_ms`, default 150): faster edges are physically
  impossible (Q4 = 3.1 m³/h at 0.5 L/edge is 0.58 s apart).
* **plausibility**: outside 50..3250 mV for 0.5 s the mode becomes `Fault`
  and nothing counts; after 2 s of plausible signal it goes to `Resync`,
  which waits until the signal is clearly beyond one threshold for `dwell_ms`,
  takes that as the current sector without counting, then `Run`. Between the
  thresholds nothing is decided, so a half-fitted adapter or a disc parked on
  the boundary costs at most one missed edge, never a phantom one.
* **pause** (`POST /api/v1/meter/pause`): `Paused`, e.g. to take the adapter
  off; resuming goes through `Resync`, so reattaching cannot produce an edge.

The detector also starts in `Resync`, so a reboot never creates a phantom
edge. Volume per edge (`ml_per_edge`, default 500) depends on the meter: the
first one turns its disc once per litre, and both edges count.

**Leak watch:** a *run* is a series of edges with never 10 minutes of quiet
between them. Once the edges of one run span an hour, `continuous_flow` is
set (published as a `binary_sensor`); 10 quiet minutes clear it. The flow
figure alone cannot do this: with 0.5 L per edge and a 10 s window it is 0 or
≥ 3 L/min, so a dripping tap stays invisible in it.

## Edge log

The `edgelog` partition (256 KB at 0x380000) is a ring of 4 KB sectors of
8-byte slots: `type | 48-bit payload | crc8`. Edges are stored as uptime ms;
one boot record per boot and one epoch record once SNTP has set the clock,
so edges before time sync still get wall-clock time. Append-only, a sector is
erased only when the ring comes round again, a torn slot fails its crc and is
skipped. Capacity ~32 700 edges (~16 m³). Writes go through a queue to a task
of their own, so the sampler never blocks on flash.

## The live oscilloscope

The WebSocket carries ADC samples from a ring buffer with replay: a reconnect
continues the graph instead of clearing it. Opening the Tuning tab starts a
fresh trace (it asks for no replay), so nothing from before is shown.

In the **Tuning** tab you see the raw signal in real time, with both
thresholds as dashed lines, every detected edge as a vertical stroke and
rejected crossings marked at the top. The stream only runs while the tab is
open, at 50 Hz.

What it took to make it smooth:

* up to 10 samples (100 ms) bundled into one JSON array frame, `TCP_NODELAY`
  set in the pre-handshake callback;
* every client gets its own refcounted payload copy via
  `httpd_ws_send_data_async`, at most 8 in flight; a failed send closes the
  client;
* `send_wait_timeout` 3 s (was 20) and TCP keepalive, so a stalled browser
  tab can block neither other clients nor OTA;
* WiFi power save off while the scope is on;
* the UI plays out on its own clock 300 ms behind the device timestamps and
  draws against a time axis; a watchdog reconnects after 3 s of silence.

## Web UI

Tabs **Meter** (reading, flow, water draws from the edge log with CSV export),
**Tuning** (scope, current/min/max/swing/
rejected, thresholds, edge filter, volume per edge), **Settings** (pause card,
match register, WiFi dialog, MQTT, radio transmit power with Apply, time server
and timezone, device name, auth) and
**System** (status LED legend, OTA, state, restart, factory reset).

WiFi change is test-before-store: from the setup network the device tries the
credentials, reports the new IP in `net.test`, stores and restarts after
30 s. From an existing network it probes the new one, returns to the old one
to report the IP (the networks may be on different subnets), then commits
after 12 s. The UI redirects to the new IP in both cases.

## Time

SNTP starts once the station has an address. Server from the settings
(default `pool.ntp.org`), with a built-in one always behind it
(`time.cloudflare.com`, or `pool.ntp.org` when that is the configured one);
left empty, both slots are built in. Synced every hour. The device counts in
UTC; the timezone (IANA name for the browser, POSIX rule for `TZ`) only sets
how times are shown. The server that last answered is reported in
`/info` → `time.server` and in the MQTT status.

## MQTT

Topic root `<base>` from the settings, default `flowtick/<device name>`.

| Topic | Retained | Content |
|---|---|---|
| `<base>/status` | yes | `online` / `offline` on change and every minute; LWT. `offline` while the detector is in `Fault`, and ~45 s after the device vanishes (keepalive 30 s) |
| `<base>/state/total` | yes | reading in m³, every 10 s |
| `<base>/state/flow` | yes | L/min over the last 10 s, every 10 s |
| `<base>/state/continuous_flow` | yes | `On` / `Off`, every 10 s |
| `<base>/sensor` | yes | JSON every minute: `mode`, firmware, uptime, boot reason, heap, network (hostname, SSID, RSSI, channel, TX power, MAC, IP, netmask, gateway, DNS), time (synced, server, timezone) |

With discovery on, Home Assistant gets one device with *Meter reading*
(`water`, `total_increasing`), *Flow*, *Continuous flow* (`problem`) and a
diagnostic *Status* whose attributes are the JSON above. Values are always
published; whether they are current is what `<base>/status` says; an empty
retained payload would erase the reading at the broker.

## Status LED

One LED (GPIO15), pattern repeats every 10 s: 1 flash online, 2 flashes WiFi
configured but not connected, 3 flashes online but sensor signal
implausible; plus one short flash per edge. Solid = booting / erase armed,
5 Hz = setup network, mostly-on = OTA, 10 Hz = fatal. See
[`src/indicator.h`](src/indicator.h).

## API

All endpoints honour the configured basic auth (off by default).

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | the web interface |
| GET | `/api/v1/info` | overall status: `firmware`, meter (incl. `online`, `mode`, `rejected`, `dwell_ms`, `min_edge_ms`, `continuous_flow`, `flow_run_s`), net (incl. `test`), system (`tx_power_dbm`, `ntp_server`, `tz_name`), time, mqtt |
| GET | `/api/v1/meter` | reading, flow, raw value |
| POST | `/api/v1/meter/total` | `{"m3":44.2670}`: match the mechanical register |
| POST | `/api/v1/meter/pause` | `{"on":true\|false}`: stop / resume counting |
| GET | `/api/v1/edges?limit=N` | edge log, oldest first: `[[t_ms, synced, boot], …]` (default 2000, `0` = all) |
| DELETE | `/api/v1/edges` | erase the edge log |
| GET | `/api/v1/tuning` | current tuning |
| POST | `/api/v1/tuning` | `{"thr_hi_mv":…,"thr_lo_mv":…,"ml_per_edge":…,"dwell_ms":…,"min_edge_ms":…}`, missing fields unchanged |
| POST | `/api/v1/minmax/reset` | restart the swing window |
| POST | `/api/v1/scope` | `{"on":true}`: live stream |
| GET | `/api/v1/samples?since=N` | pull the ring instead of having it pushed |
| GET | `/api/v1/ws` | WebSocket: sample stream |
| GET | `/api/v1/wifi/scan` | networks in range, strongest first |
| POST | `/api/v1/wifi/connect` | `{"ssid":…,"password":…}`: test, then store (see above); progress in `/info` → `net.test` |
| GET/POST | `/api/v1/config/mqtt` | MQTT settings: `enabled`, `host`, `port`, `tls`, `tls_insecure`, `ca_cert` (empty with TLS = built-in CA bundle), `client_id`, `auth`, `user`, `password` (absent = unchanged), `base`, `discovery`, `discovery_prefix` |
| POST | `/api/v1/config/device` | `{"name":…}`: device / host name |
| POST | `/api/v1/config/auth` | `{"enabled":…,"user":…,"password":…}`: web UI auth |
| POST | `/api/v1/config/system` | `{"tx_power_dbm":17}`: transmit power (0 = maximum); `{"ntp_server":"pool.ntp.org","tz_name":"Europe/Berlin","tz_posix":"CET-1CEST,M3.5.0,M10.5.0/3"}`: time server (empty: built-in only; a built-in fallback always stands behind it) and timezone (name and POSIX rule together) |
| POST | `/api/v1/ota` | firmware.bin as `application/octet-stream`; header `X-Confirm: ui` when the caller will confirm the new image itself |
| GET | `/api/v1/ota/status` | running slot and version, `pending_verify`, `ui_confirmed`, and during the trial period `probation` (`uptime`, `sampler`, `network`, `web_server`, `page_needed`, `page`, `left_s`, `window_s`) |
| POST | `/api/v1/ota/confirm` | the update page confirms the new image (see [../docs/firmware.md](../docs/firmware.md#updates-and-rollback)) |
| GET | `/api/v1/selftest` | `{"ok":true}`, answered only over loopback for the image's self-test; 404 from anywhere else |
| POST | `/api/v1/restart` | restart |
| POST | `/api/v1/reset` | factory reset |
| GET | `/api/v1/health` | tasks, stack, heap, reset reason |
| GET/DELETE | `/api/v1/coredump` | download / erase the stored coredump |

## Decisions

**Oneshot ADC instead of continuous/DMA.** At Q3 = 2.5 m³/h the flow is at most
41.7 L/min; at 0.5 L per edge that is ~1.4 edges per second. 100 Hz is a
factor of 70 above that. DMA would mean a ring buffer and interrupt handling
for a signal that changes about once a second.

**Median over 8 samples, not the mean.** The median kills single outliers
without smearing the edge. A mean would blur exactly the edge that matters.

**`CONFIG_FREERTOS_HZ=1000`.** Not optional. At the default of 100 Hz the tick
resolution would equal the sampling period, and the jitter would be 100 %.

**Reading written to NVS once a minute if it changed, not per pulse.** Per
pulse the flash would be worn out within months; once a minute is about two
erases per NVS page and day even with water running non-stop (~135 years to
100 000 cycles). Cost: at most a minute of consumption on a power cut. Before
every deliberate restart (OTA, restart, recovery) it is written as well.

**Reading saved before every deliberate restart.** A shutdown handler in
`meter` writes it from `esp_restart()`. Without it every OTA lost up to the
last save interval, and Home Assistant reads a reading that went backwards
(`total_increasing`) as a meter swap.

**Reading as `uint64` in millilitres.** `uint32` would overflow at ~4,294 m³.
For a project meant to outlive several meter generations, not a theoretical
problem.

**Thresholds are rejected if `thr_hi <= thr_lo`.** Such a hysteresis is none:
it would flip on every sample and drive the counter up by cubic metres within
seconds. Better to reject than to puzzle later over where the consumption came
from.

## Pinout

See [`src/board.h`](src/board.h). In short: ADC on **GPIO1**, status LED
GPIO15, BOOT GPIO9, IR LED tied to 3V3.

On the C6, ADC1 sits on GPIO0..GPIO6 and there is no ADC2; of those, only
GPIO0..GPIO3 are broken out on the Super Mini and free of boot duties.

`LED_ACTIVE_LOW` is verified as `false` (active high) on this board. The Super Mini clones are not consistent
about it. If the LED blinks inverted, it is this switch and nothing else.
