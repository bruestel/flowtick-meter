/*
   The meter: a reflective sensor over the optical scanning disc of the
   Allmess +m module register.

   The disc turns in proportion to the flow and carries one bright and one
   dark sector. A reflective sensor above it sees a square wave; every edge is
   a fixed volume.

   Why build it at all: the original PM +m module does the same, but has a
   glued-in lithium cell with a 13-year life. With a six-year calibration
   period per meter that means buying a new one every twelve years, and the
   module bay is identical across the whole Allmess family -- a sensor built
   once survives every meter swap.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace meter {

struct Config {
  /* ADC pin. On the ESP32-C6, ADC1 sits on GPIO0..GPIO6 and there is no
     ADC2; of those, the Super Mini breaks out GPIO0..GPIO3. */
  int adc_gpio = 1;

  /* Switch the IR LED through a GPIO, or -1 if it is tied to 3V3.
     Tied is the normal case: there is no ambient light inside the closed
     module bay, and the few mA through R1 are irrelevant to the power supply. */
  int ir_gpio = -1;
};

/* What lives in NVS and can be adjusted at runtime.

   Kept apart from the meter reading because it behaves differently: the
   thresholds change a few times during tuning and then never again, the
   reading changes all the time. Writing both at the same rate would be too
   often for one and too rarely for the other. */
struct Tuning {
  /* Defaults from tuning the first adapter (R1 560 Ω, R2 1.56 kΩ: dark
     ~500 mV, chrome ~2900 mV), so a freshly flashed device counts on that
     meter without tuning. The factory reset keeps stored tuning anyway. */
  uint32_t thr_hi_mv   = 2200; // upper switching threshold
  uint32_t thr_lo_mv   = 1200; // lower switching threshold
  uint32_t ml_per_edge = 500;  // volume per detected edge
  /* A crossing counts only once the signal has stayed beyond the threshold
     this long -- a spike, a flash of light or a bouncing transition does not. */
  uint32_t dwell_ms = 80;
  /* Edges closer together than this are physically impossible: at the
     meter's overload flow Q4 = 3.1 m³/h and 0.5 L per edge they are 0.58 s
     apart. Faster ones wait until this has passed, so noise cannot run away. */
  uint32_t min_edge_ms = 150;
};

/* What the edge detector is doing. */
enum class Mode : uint8_t {
  Resync, // finding out which sector the sensor sees, without counting
  Run,    // counting
  Fault,  // signal implausible (sensor off, adapter removed, light in): not counting
  Paused, // paused by hand, e.g. to take the adapter off
};
const char *to_string(Mode m);

struct State {
  uint64_t total_ml  = 0;
  float    flow_lmin = 0.0f;
  uint64_t edges     = 0;

  int mv     = 0;
  int mv_min = 0;
  int mv_max = 0;

  /* Whether the signal is plausible at all. A sensor that has come loose or
     whose LED has burnt out delivers a constant rail value -- and otherwise
     looks just like a household that uses no water. */
  bool online = false;

  Mode     mode     = Mode::Resync;
  uint32_t rejected = 0; // crossings that did not last dwell_ms: spikes, bounce

  /* Leak watch. A run is edges with never 10 minutes of quiet between them;
     continuous_flow is set once the edges of one run span an hour. At 0.5 L
     per edge a dripping tap is invisible to the flow figure (0 or >= 3 L/min
     in a 10 s window), but not to this. */
  bool     continuous_flow = false;
  uint32_t flow_run_s      = 0; // span of the current run, 0 if none
};

bool begin(const Config &cfg);

State state();

/* Samples taken since boot: proof that the sampler task is alive. */
uint32_t sample_count();
Tuning   tuning();

/* Thresholds and litre factor. Written to NVS immediately, because they
   rarely change and a lost calibration costs more than a flash write. */
bool set_tuning(const Tuning &t);

/* Set the reading to match the mechanical register. Absolute, not a
   correction: what you read off is a value, and a correction sent twice
   would double the error instead of fixing it. */
bool set_total_m3(double m3);

void reset_minmax();

/* Stop counting, e.g. to take the adapter off. On resume the detector first
   looks which sector it sees and only then counts again, so putting the
   adapter back cannot produce an edge. */
void set_paused(bool on);

/* Live stream of the raw signal. Off while nobody is watching. */
void set_scope(bool on);
bool scope();

/* From the main loop. Computes the flow and writes the reading to NVS once a
   minute if it has changed -- not on every pulse, which would wear out the
   flash within months. NVS spreads the writes over its pages: even with water
   running day and night that is about two erases per page and day, some 135
   years to the rated 100 000. A power cut costs at most a minute of
   consumption. */
void tick(int64_t now_us);

/* Every sample while the stream is on. Called from the sampler task and
   therefore must not block. */
using SampleSink = std::function<void(int mv, bool edge, int64_t t_us)>;
void set_sample_sink(SampleSink sink);

/* Every detected edge, whether or not the stream is on. Also called from the
   sampler task, so it must not block either. */
using EdgeSink = std::function<void(int64_t t_us)>;
void set_edge_sink(EdgeSink sink);

/* Pre-rendered for the HTTP layer. */
std::string state_json();
std::string tuning_json();

/* Accepts {"thr_hi_mv":…,"thr_lo_mv":…,"ml_per_edge":…,"dwell_ms":…,
   "min_edge_ms":…}; missing fields stay as they were. */
bool set_tuning_json(const std::string &body, std::string *error);

} // namespace meter
