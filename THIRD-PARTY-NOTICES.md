# Third-party notices

flowtick-meter is licensed under the Apache License, Version 2.0; see
[LICENSE](LICENSE). Everything in this repository (firmware source, CAD
models, documentation and images) is under that licence.

The **firmware images** are not only this source. Building links in the ESP-IDF
framework and the components listed below, so a released `.bin` carries other
people's code under their licences. This file records what and whose, which is
what those licences ask for in return.

Nothing here is vendored into the repository. The components are fetched at
build time by the IDF component manager, pinned by
[`firmware/dependencies.lock`](firmware/dependencies.lock), and the framework
comes from the PlatformIO platform pinned in
[`firmware/platformio.ini`](firmware/platformio.ini).

## What the images contain

| Component | Version | Licence | Source |
| :--- | :--- | :--- | :--- |
| ESP-IDF | 6.0.1 | Apache-2.0 | [espressif/esp-idf](https://github.com/espressif/esp-idf) |
| FreeRTOS kernel | 10.5.1 | MIT | bundled with ESP-IDF |
| lwIP | 2.2.0 | BSD-3-Clause | bundled with ESP-IDF |
| Mbed TLS | 4.0.0 | Apache-2.0 (see below) | bundled with ESP-IDF |
| Wi-Fi and PHY libraries | with ESP-IDF 6.0.1 | Apache-2.0 (see below) | [espressif/esp32-wifi-lib](https://github.com/espressif/esp32-wifi-lib) |
| CA certificate bundle | with ESP-IDF 6.0.1 | MPL-2.0 (data) | Mozilla root store, via ESP-IDF `esp_crt_bundle` |
| cJSON | 1.7.19~2 | MIT | [espressif/cjson](https://components.espressif.com/components/espressif/cjson) |
| mDNS | 1.13.1 | Apache-2.0 | [espressif/mdns](https://components.espressif.com/components/espressif/mdns) |
| esp-mqtt | 1.1.0 | Apache-2.0 | [espressif/mqtt](https://components.espressif.com/components/espressif/mqtt) |

All of these are compatible with distributing the combined firmware under
Apache-2.0.

**Mbed TLS** is offered under Apache-2.0 *or* GPL-2.0-or-later. The Apache-2.0
option is the one that applies here.

**The Wi-Fi and PHY libraries** are shipped by Espressif as pre-compiled
binaries under Apache-2.0. The licence is permissive, but their source is not
published, so they cannot be rebuilt from source the way everything else can.

**The CA certificate bundle** is the list of root certificates from Mozilla's
CA store, which ESP-IDF converts and embeds for TLS server verification (used
by `mqtts://` with "public authorities"). The certificate data is distributed by
Mozilla under the Mozilla Public License 2.0; it is embedded unmodified.

## Where the firmware came from

The infrastructure components (networking, configuration storage, web server,
MQTT, OTA with rollback, watchdog, coredump and BOOT-button recovery) were
taken from the author's own `bsh-dbus-idf` project and adapted. Only the
author's own code was taken; none of that project's third-party code is part
of this repository.
