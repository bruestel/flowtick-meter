/*
   HTTP server: REST API, WebSocket live scope, embedded UI.

   Infrastructure taken from the author's bsh-dbus-idf project.
   The HTTP, auth, OTA and WebSocket machinery is taken over unchanged; what
   was replaced is the application-specific API surface.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace appweb {

/* Everything the status endpoint reports. Supplied by the application, so this
   component need not know anything about the ADC or the register. */
struct Status {
  /* Register */
  double   total_m3  = 0;
  float    flow_lmin = 0;
  uint64_t edges     = 0;

  /* Raw signal. mv_min and mv_max are what step 0 needs: the swing over the
     observation window, resettable from the interface. */
  int mv     = 0;
  int mv_min = 0;
  int mv_max = 0;

  /* Tuning */
  uint32_t    thr_hi_mv   = 0;
  uint32_t    thr_lo_mv   = 0;
  uint32_t    ml_per_edge = 0;
  bool        scope       = false; // live stream active
  bool        online      = false; // raw signal inside the plausible range
  std::string mode;                // edge detector: resync / run / fault / paused
  uint32_t    rejected        = 0; // crossings too short to count
  bool        continuous_flow = false;
  uint32_t    flow_run_s      = 0;
  uint32_t    dwell_ms        = 0;
  uint32_t    min_edge_ms     = 0;

  std::string net_state;
  std::string ip;
  std::string ssid;
  int8_t      rssi    = 0;
  uint8_t     channel = 0;

  /* Which board this image drives, and where the sensor is attached. The
     interface shows it, so troubleshooting does not start with unscrewing the
     housing. */
  std::string board;
  int         pin_adc      = -1;
  int         pin_ir       = -1;
  int         pin_led      = -1;
  bool        led_inverted = false;
};

/* The meter layer is application-specific and is handed in rather than
   depended on from here -- this component sticks to HTTP. */
struct MeterApi {
  /* Reading, flow, raw value, swing -- as ready-made JSON. */
  std::function<std::string()> state_json;

  /* Thresholds and litre factor. */
  std::function<std::string()>                                     tuning_json;
  std::function<bool(const std::string &body, std::string *error)> set_tuning;

  /* Align the reading with the mechanical register. */
  std::function<bool(double m3, std::string *error)> set_total;

  /* Restart the swing window. Before every new distance or resistor variant
     in step 0. */
  std::function<void()> reset_minmax;

  /* Stop counting to take the adapter off; resuming resynchronises first. */
  std::function<void(bool on)> set_paused;

  /* Switch the raw-signal live stream on and off. Off while nobody is
     watching: pushing 50 samples a second through a WebSocket nobody has
     open is wasted airtime. */
  std::function<void(bool on)> set_scope;

  /* The edge log, newest `limit` entries (0 = all), handed out one JSON
     element at a time so the whole log never has to sit in RAM. `emit`
     returns false once the client is gone. Returns the total logged. */
  std::function<size_t(size_t limit, const std::function<bool(const std::string &)> &emit)> edges;
  std::function<bool()>                                                                     clear_edges;
};

/* Health and post-mortem, handed in for the same reason as the meter API:
   this component sticks to HTTP. */
struct DiagnosticsApi {
  std::function<std::string()>                              health_json;
  std::function<size_t()>                                   coredump_size;
  std::function<bool(size_t offset, void *buf, size_t len)> read_coredump;
  std::function<bool()>                                     erase_coredump;
};

using StatusProvider = std::function<Status()>;

/* Called with true when a firmware upload starts and with false when it fails
   -- so the application can signal it without this component knowing anything
   about LEDs. */
using OtaHook = std::function<void(bool active)>;

bool begin(StatusProvider provider, OtaHook ota_hook = {}, MeterApi meter = {}, DiagnosticsApi diagnostics = {});

/* One sample for every connected scope. Never blocks: a client that cannot
   keep up loses samples, the sampler does not lose its time. It catches up
   from the ring on reconnect.

   `edge` marks the sample at which an edge was detected -- so the graph shows
   not only the signal but also what the firmware makes of it. That is exactly
   how you see whether the thresholds are right. */
void publish_sample(int mv, bool edge, int64_t t_us);

/* For the firmware update's trial period (main.cpp).

   self_check() asks the device's own server for /api/v1/selftest over
   loopback and is true when it answers -- proof that a new image can still be
   reached and updated again. Blocks for up to two seconds; call it from a task
   of its own.

   ui_confirmed() is true once the update page has come back after the restart
   and confirmed the new image (POST /api/v1/ota/confirm). */
bool self_check();
bool ui_confirmed();

/* The state of a new image's trial period (probation.cpp), shown in
   /api/v1/ota/status so a failing self-test can be read without a cable. A
   default-constructed one (active = false) clears it. */
struct Probation {
  bool active      = false;
  bool uptime      = false;
  bool sampler     = false;
  bool network     = false;
  bool web_server  = false;
  bool page_needed = false; // uploaded from the web interface
  bool page        = false; // confirmed by it, or not needed
  int  left_s      = 0;
  int  window_s    = 0;
};
void set_probation(const Probation &p);

} // namespace appweb
