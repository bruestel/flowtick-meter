# Firmware

The firmware lives in [`firmware/`](../firmware/). It is written in C++20 on
ESP-IDF 6.0.1 and built with PlatformIO. Target is the ESP32-C6 Super Mini.
Components, internals and the full HTTP API are described in
[firmware/README.md](../firmware/README.md).

## Building and flashing

```bash
cd firmware
pio run -e supermini-c6                                    # build
pio run -e supermini-c6 -t upload --upload-port /dev/ttyACM0
pio device monitor
```

* **Plug the board straight into the computer**, not through a chain of USB
  hubs. Behind hubs the C6 can go into a brownout loop while flashing.
* The flash size is pinned to **4 MB** in `platformio.ini`. Without that the
  upload writes an 8 MB header and the bootloader stops with an assert.
* After the first flash, updates go over the air: use *System* in the web
  interface, or run

  ```bash
  curl -X POST -H 'Content-Type: application/octet-stream' \
       --data-binary @.pio/build/supermini-c6/firmware.bin \
       http://192.168.1.50/api/v1/ota
  ```

  A new image runs on probation and is only kept once it has proven itself,
  see [Updates and rollback](#updates-and-rollback).
* **Version:** set by hand in `firmware/version.txt`. In a git checkout,
  `git describe` takes its place.

## First start

No credentials are compiled in. Without WiFi credentials the device opens its
own open network (no password) with a captive portal. Enter WiFi and,
optionally, MQTT there. Once the device has joined your WiFi, the setup
network stays up for another 120 s, so the phone that set it up can still read
the new address, and then closes.

The setup network is named after the device: `flowtick-xxxxxx` (the last three
bytes of the MAC) on a new device, or the name you gave it. You can change
that name, which is also the hostname, under *Settings*, for example to
`flowtick-garden`. On the home network the device announces itself via mDNS,
so `http://flowtick-garden.local` works as well as the IP address on systems
that resolve `.local` names.

Home Assistant identifies the device by `water-meter-xxxxxx`, an ID from the
time before the project was renamed. It is kept so that existing entities
survive the rename.

## Web interface

Four tabs:

| Tab | Content |
|---|---|
| **Meter** | reading, flow, continuous-flow indicator, *Water draws* (edge log grouped into draws, CSV export, clear) |
| **Tuning** | live oscilloscope of the raw signal, Current/Min/Max/Swing/Rejected, thresholds, edge filter, volume per edge |
| **Settings** | *Taking the adapter off* (pause counting), *Match the mechanical register*, WiFi, MQTT, *Radio* (transmit power), *Time* (time server and timezone), device name, password protection |
| **System** | status LED legend, firmware update, status, restart, factory reset |

Every tab has its own address (`#meter`, `#tuning`, `#settings`, `#system`), so
you can bookmark one directly. Top right, a five-bar indicator shows the WiFi
signal strength, with the dBm value in the tooltip, plus the MQTT state and the
firmware version.

### Changing WiFi

*Settings → WiFi* opens a dialog with the networks in range. Credentials are
**tested first, then stored**:

* **From the setup network:** the device joins the new network as a trial,
  reports success and its new IP in the dialog, stores the credentials and
  restarts after 30 s. The page then redirects to the new IP.
* **From an existing network:** the device tries the new network, returns to
  the old one to report its new IP, and then switches for good. The two
  networks may be on different subnets. The page redirects to the new IP.

Wrong credentials are never stored, so the device cannot lock itself out.
While a successful change is still being applied, a second one is refused
with "still being applied; try again shortly", so it cannot slip in untested.

The station runs at 20 MHz with WiFi 6 (802.11ax) enabled: on 40 MHz the C6
falls back to WiFi 4, and a water meter needs reach, not throughput.

### Transmit power

*Settings → Radio* sets the transmit power in dBm (0 = maximum). **Apply**
takes effect immediately, without a restart, and is stored. Lower it as far as
reception allows. After a brownout, the firmware steps it down by itself.

### Time

*Settings → Time*: the time server defaults to `pool.ntp.org`. A built-in
server always stands behind it (`time.cloudflare.com`, or `pool.ntp.org` when
that is already the configured one). If you leave the field empty, both slots
use built-in servers. The clock syncs every hour.

The device counts in UTC. The timezone, picked from a list (default
Europe/Berlin), only sets how times are shown in the web interface. The CSV
export and MQTT stay in UTC.

## How edges are counted

The sampler reads the ADC (GPIO1) at 100 Hz, as the median of 8 readings. The
edge detector does more than hysteresis, so that neither interference nor
taking the case off makes up water:

1. **Hysteresis.** Two thresholds; for the Allmess meter, 2200 / 1200 mV.
2. **Dwell** (`dwell_ms`, 80 ms). A crossing only counts once the signal has
   stayed beyond the threshold this long. Shorter ones are discarded and
   counted as *Rejected*.
3. **Minimum gap** (`min_edge_ms`, 150 ms). Edges closer than this are
   physically impossible and wait. A crossing that falls back before the gap
   has passed is discarded and counted as *Rejected* too.
4. **Plausibility.** Outside 50–3250 mV means a rail: the sensor is loose, the
   LED is dead, or light gets in. After 0.5 s of this the detector goes to
   **Fault** and stops counting. After 2 s of plausible signal comes
   **Resync**. The detector only decides which sector it sees once the signal
   has been clearly below the lower or above the upper threshold for the dwell
   time. In between, for example while the case is being put back or when the
   disc has stopped on the boundary, nothing is decided. At worst one edge
   (0.5 L) is missed once, but a phantom edge cannot occur.
5. **Pause.** A button in the web interface, or `POST /api/v1/meter/pause`,
   for example to take the case off. Resuming also goes through Resync.

The detector also starts in Resync, so a restart never counts a phantom edge.

The mode (`run`, `resync`, `fault`, `paused`) and the number of rejected
crossings are in `/api/v1/info` and in the Tuning tab. Tuning itself is
covered in [tuning.md](tuning.md).

## Keeping the reading across restarts

The reading is a `uint64` in millilitres. It is written to NVS **every minute
if it has changed**, not on every edge, which would wear out the flash within
months. A power cut costs at most a minute of consumption. Even with water
running day and night, that is about two erase cycles per NVS page and day,
roughly 135 years to the rated 100 000.

Before every deliberate restart (OTA, restart button, recovery), the reading
is saved as well. Home Assistant treats a reading that goes backwards
(`total_increasing`) as a meter swap.

Thresholds and the volume per edge are stored immediately. They rarely change,
and losing one costs more than a flash write.

**Matching the register:** *Settings → Match the mechanical register* takes the
reading in m³ as an absolute value, not a correction. Sending it twice still
gives the right value.

## Updates and rollback

A new image starts **on probation**. It is kept only once it has shown,
by itself:

* 30 s up without a crash or watchdog reset,
* the sampler still taking samples,
* a network: connected to your WiFi when credentials are stored (the setup
  network only counts on a device that has none, since an image that can no
  longer join your WiFi is out of reach at the meter),
* its own web server answering, checked over loopback (127.0.0.1), so an image
  that could not be updated again is never kept.

Uploaded from *System → Firmware update*, the page also waits for the device
to come back and **confirms** the new image; both are needed, within 120 s.
Keep the page open until it reports the result: closed before it has
confirmed, the device rolls back once the 120 s are up. Uploaded with `curl`,
the self-test decides alone, within 60 s. A device whose WiFi credentials are
stored but whose home network is down while you update it, even over the setup
network, always rolls back: the new image has to show that it can join that
network. Whatever is still missing shows in `GET /api/v1/ota/status` under
`probation`. If time runs out, the device restarts into the previous firmware.

## Recovery and factory reset

Two ways back, for different situations:

| | How | Erases | Keeps |
|---|---|---|---|
| **BOOT button** | hold BOOT for 5 s while the device runs; the LED goes solid after the first second and stays on while the hold counts, flashes three times and the device restarts. Held while plugging in, BOOT enters download mode instead and erases nothing | WiFi credentials and the web interface password | everything else |
| **Factory reset** | *System* tab, *Erase all settings* | WiFi, password, device name, MQTT, transmit power, time server and timezone | meter reading, tuning (thresholds, edge filter, volume per edge), edge log |

The BOOT button is the way in when the device no longer joins your WiFi or
the password is lost: afterwards it opens its setup network again, under its
current device name. A hold
that is released early changes nothing. The factory reset leaves the meter
counting, because losing the reading or the tuning costs more than entering
WiFi again.

## Edge log

Every edge also goes into its own flash partition, `edgelog`. It is 256 KB and
holds ~32 000 edges, which is ~16 m³ at 0.5 L per edge. The log is a ring of
4 KB sectors with 8-byte entries, append-only, each entry with a CRC. A sector
is only erased when the ring comes back round to it. Wear is spread evenly,
and a power cut in the middle of a write spoils at most one entry.

Entries store the uptime. Once SNTP has set the clock, a per-boot epoch entry
is added, so edges from before the time sync also get a real time.

*Meter → Water draws* groups edges less than 60 s apart into draws (start,
duration, litres), over the whole log. *Download CSV* writes the raw log, one
row per edge with its time (UTC), so it can be grouped differently elsewhere.

## Status LED

One LED (GPIO15). The pattern repeats every **10 s**; count the flashes:

| Pattern | Meaning |
|---|---|
| 1 flash | all good, online |
| 2 flashes | WiFi configured but not connected |
| 3 flashes | online, but the sensor is in Fault |
| plus 1 short flash per edge | water is flowing |
| solid | booting / BOOT held (erase armed) |
| even 5 Hz | setup network active |
| mostly on, short gaps | firmware update, do not unplug |
| 10 Hz | fatal error |

The legend is also in the System tab. The green BAT LED on some Super Mini
boards hangs off the charger circuit and cannot be switched off in firmware.
Desolder it if it bothers you.

## Live oscilloscope

The Tuning tab shows the raw signal in real time. Both thresholds appear as
dashed lines, every detected edge as a vertical line, and rejected crossings
are marked at the top. The stream (50 Hz) only runs while the tab is open, and
starts empty each time you open it.

To keep it smooth:

* Samples go out in bundles of up to 10 (100 ms), on a socket with
  `TCP_NODELAY`.
* The page plays back on its own clock, 300 ms behind the device, and draws
  along the time axis rather than by arrival.
* WiFi power save is off while the scope runs.
* A watchdog reconnects after 3 s of silence.

The web server closes clients it cannot send to (3 s timeout, TCP keepalive),
so a stuck browser tab blocks neither other clients nor an OTA update.

## MQTT

Under *Settings → MQTT*:

* **Publish to a broker:** switches MQTT on or off. The settings are kept
  either way.
* **Broker:** host, `mqtt://` or `mqtts://` (the default port follows: 1883 /
  8883), and an optional client ID.
* **Server certificate** (mqtts only):
  * **Public authorities (built in):** the firmware carries Mozilla's root CA
    list (~140 roots). This fits brokers with a certificate from Let's
    Encrypt or a cloud broker. The host must then be the name in the
    certificate, not an IP.
  * **Own CA certificate:** paste a PEM, for a self-signed broker or your own
    CA.
  * **Do not check:** encrypted, but anybody could pose as the broker.
* **Login:** optional. When it is off, the device connects anonymously and
  deletes any stored credentials on Save. A stored password is kept as long
  as the field stays empty.
* **Topics:** the topic root, which defaults to `flowtick/<device name>`,
  e.g. `flowtick/flowtick-garden`; set it to something shorter if you like.
* **Home Assistant discovery:** on/off, plus the discovery prefix. Switched
  off, the device removes its entities from Home Assistant on the next
  connect.

Published topics, all retained, with `<base>` the topic root (here set to
`flowtick/garden`):

| Topic | Content |
|---|---|
| `<base>/status` | `online` / `offline`; the last will says `offline` |
| `<base>/state/total` | reading in m³ |
| `<base>/state/flow` | flow in L/min (10 s window) |
| `<base>/state/continuous_flow` | `On` / `Off` |
| `<base>/sensor` | status JSON, every minute |

**Availability.** `<base>/status` is `offline` while the edge detector is in
**Fault** (0.5 s implausible, back after 2 s steady). `resync` and `paused`
count as `online`, because the reading stays valid. The status LED (3 flashes)
follows the same rule. If the device disappears altogether, the broker sends
its last will, `offline`, after about 45 s (MQTT keepalive 30 s).

On every connect the device also publishes empty retained payloads to the
topics earlier firmware used (`<base>` itself and `<base>/state/mv`,
`edges`, `status`), which removes them from the broker.

**Status JSON** on `<base>/sensor`:

```json
{"mode":"run","firmware":"0.10.0","uptime_s":1234,"boot_reason":"power on",
 "free_heap":264820,"min_free_heap":255888,
 "network":{"hostname":"flowtick-garden","ssid":"MyWiFi","rssi_dbm":-67,
            "channel":6,"tx_power_dbm":17,"mac":"aa:bb:cc:dd:ee:ff",
            "ip":"192.168.1.50","netmask":"255.255.255.0",
            "gateway":"192.168.1.1","dns":"192.168.1.1"},
 "time":{"synced":true,"server":"pool.ntp.org","timezone":"Europe/Berlin"}}
```

## Web interface login

Off by default. With a password set, every page and API call needs it (HTTP
Basic auth). After 5 wrong attempts further logins are refused for 10 s,
which makes guessing slow without locking you out for long.

## Home Assistant

With discovery on, the device appears with these entities:

* **Meter reading**: `device_class: water`, `state_class: total_increasing`,
  in m³.
* **Flow**: in L/min.
* **Status** (diagnostic): its state is the detector mode, and its
  attributes are the JSON above. It stays readable when the sensor is in
  Fault, and becomes unavailable after 3 minutes without an update.
* **Continuous flow** (`binary_sensor`, `device_class: problem`).

For the **energy dashboard**, add the meter reading under *Settings →
Dashboards → Energy → Water consumption*.

### Automation ideas

* **Leak warning:** use **Continuous flow**. It turns *Problem* once edges
  with never 10 minutes of quiet between them span an hour, and 10 minutes
  without an edge reset it. The flow figure alone is no good for this: at
  0.5 L per edge and a 10 s window it is either 0 or ≥ 3 L/min, so a dripping
  tap never shows. Long garden watering triggers it too, so exclude that with
  a condition in the automation.
* **Burst pipe:** flow above a level that never occurs in the household.
* **Away:** any consumption above 2 L while nobody is home.
* **Sensor:** Status in `fault` for more than a few minutes.
