/*
   The trial period of a freshly flashed image.

   A new image starts as PENDING_VERIFY. If it never marks itself valid --
   because it crashes, hangs or cannot be reached -- the bootloader falls back
   to the previous slot. A firmware that boots but cannot be reached or updated
   again is as useless as one that crashes, and there is no cable in the meter
   pit. So it is kept only once it has shown all of this itself:

     - 30 s up without a crash or watchdog reset,
     - the sampler still taking samples,
     - a network: the WiFi station connected when credentials are stored (the
       setup network only counts on a device that has none -- an image that
       can no longer join the home network is unreachable at the meter),
     - its own web server answering over loopback (appweb::self_check),

   and, when the update came from the web interface, once that page is back
   and has confirmed it. Uploaded with curl, nobody will confirm, and the
   self-test decides alone, in the shorter window.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>

namespace probation {

/* Reads whether this image is on trial and starts the web self-test. */
void begin();

/* From the main loop. Keeps the image or rolls it back once decided. */
void tick(int64_t now_us, bool station_connected);

} // namespace probation
