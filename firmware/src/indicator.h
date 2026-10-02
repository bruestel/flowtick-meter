/*
   LED signalling.

   In the meter pit the LED is the only feedback channel: no cable, no
   browser, no logs. So it has to separate exactly the cases one would
   otherwise be blind to -- above all "WiFi gone" versus "WiFi up, but no
   signal", and "no flow" versus "sensor delivers nonsense".

   The Super Mini has only one controllable LED (GPIO15), so everything has
   to be told apart on it alone. The rule for normal operation is to count
   the flashes, which repeat every 10 s:

     1 flash    all good (online)
     2 flashes  WiFi configured, but not connected
     3 flashes  online, but the sensor signal is implausible

   and on top of that one short flash per detected edge while water flows.
   Everything else has a shape of its own that cannot be mistaken for a count:
   solid (booting / erase armed), even 5 Hz (setup network), mostly-on flicker
   (firmware update), 10 Hz (fatal).

   A second, activity LED is supported by the code but not fitted
   (PIN_LED_ACTIVITY = -1), which board::has_led() catches anyway.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>

namespace indicator {

enum class System : uint8_t {
  Booting,      // solid on until the first state is known
  ApMode,       // even 5 Hz -- waiting to be configured
  Disconnected, // 2 flashes, pause -- WiFi configured but not associated
  Connected,    // 1 flash, pause -- online, no edge seen yet
  Nominal,      // 1 flash, pause -- online, edges arriving (same as Connected:
                //   the per-edge flashes already show that it counts)
  Fatal,        // 10 Hz
  Ota,          // mostly on, short gaps -- do not remove power
  Wiping,       // solid on -- keep holding to erase the WiFi settings
};

enum class Sensor : uint8_t {
  Silent,  // nothing extra -- no flow
  Healthy, // one extra flash per detected edge
  Garbage, // 3 flashes instead of 1 while online: signal outside the
           // plausible range, i.e. sensor loose, LED dead or wiring wrong
};

void begin();

void   set_system(System s);
System system();
void   set_sensor(Sensor s);

/* One flash on top of the pattern. Cheap, may be called for every edge. */
void note_edge();

/* Drives the patterns. Call regularly from the main loop. */
void tick(int64_t now_us);

const char *to_string(System s);
const char *to_string(Sensor s);

} // namespace indicator
