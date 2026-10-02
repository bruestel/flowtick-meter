/*
   LED signalling.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "indicator.h"
#include "board.h"

namespace indicator {
namespace {

/* Patterns are 40 slots of 50 ms, i.e. a 2 s cycle, LSB first. Writing them as
   bit literals keeps the shape of each pattern readable in the source and makes
   it obvious that they are distinguishable from one another. */
constexpr int64_t kSlotUs = 50000;
constexpr int     kSlots  = 40;

/* The counted patterns (1, 2 or 3 flashes) repeat only every 10 s: they
   report a steady state, and a light going off every other second in a dark
   meter pit is more distraction than information. The flash group sits in
   the first 2 s, the rest of the cycle is dark. */
constexpr int kCountedSlots = 200;

constexpr uint64_t bits(const char *s) {
  uint64_t v = 0;
  for (int i = 0; i < kSlots && s[i]; i++)
    if (s[i] == '#')
      v |= (1ULL << i);
  return v;
}

/* Counted flashes are 100 ms on, 200 ms off, so they stay countable by eye;
   the long dark rest of the cycle is what makes them a group. */
//                                       0    5    10   15   20   25   30   35
constexpr uint64_t kBooting = bits("########################################");
constexpr uint64_t kApMode  = bits("##__##__##__##__##__##__##__##__##__##__");
constexpr uint64_t kOne     = bits("##______________________________________");
constexpr uint64_t kTwo     = bits("##____##________________________________");
constexpr uint64_t kThree   = bits("##____##____##__________________________");
constexpr uint64_t kFatal   = bits("#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_");
constexpr uint64_t kOta     = bits("######__######__######__######__######__");
/* Solid while the recovery hold is armed: clearly different from every
   blinking pattern, so it reads as "something is about to happen, let go if
   you did not mean it". */
constexpr uint64_t kWiping = bits("########################################");

uint64_t pattern_for(System s) {
  switch (s) {
    case System::Booting:
      return kBooting;
    case System::ApMode:
      return kApMode;
    case System::Disconnected:
      return kTwo;
    case System::Connected:
      return kOne;
    case System::Nominal:
      return kOne;
    case System::Fatal:
      return kFatal;
    case System::Ota:
      return kOta;
    case System::Wiping:
      return kWiping;
  }
  return kFatal;
}

System g_system = System::Booting;
Sensor g_sensor = Sensor::Silent;

int64_t g_cycle_start_us = 0;

/* Edge flashes are event driven rather than part of a pattern, so flowing
   water reads as irregular flicker over the calm count. note_edge() only
   raises a flag; tick() turns it into a deadline, so callers never need a
   clock. 60 ms is the shortest flash that is still plainly visible. */
int64_t           g_blip_until_us = 0;
bool              g_blip_pending  = false;
constexpr int64_t kBlipUs         = 60000;

bool slot_set(uint64_t pattern, int slot) {
  return (pattern >> slot) & 1ULL;
}

} // namespace

void begin() {
  board::led_init(board::PIN_LED_STATUS);
  board::led_init(board::PIN_LED_ACTIVITY);
}

void set_system(System s) {
  if (s == g_system)
    return;
  g_system = s;
  /* Restart the cycle so a new pattern begins at its first slot; otherwise a
     two-flash group could be entered halfway and be misread. */
  g_cycle_start_us = 0;
}

System system() {
  return g_system;
}

void set_sensor(Sensor b) {
  g_sensor = b;
}

void note_edge() {
  g_blip_pending = true;
}

void tick(int64_t now_us) {
  if (g_cycle_start_us == 0)
    g_cycle_start_us = now_us;

  if (g_blip_pending) {
    g_blip_pending  = false;
    g_blip_until_us = now_us + kBlipUs;
  }

  const int64_t elapsed = (now_us - g_cycle_start_us) / kSlotUs;

  uint64_t   pattern = pattern_for(g_system);
  const bool online  = g_system == System::Connected || g_system == System::Nominal;
  if (online && g_sensor == Sensor::Garbage)
    pattern = kThree;
  const bool counted = online || g_system == System::Disconnected;

  const int slot = static_cast<int>(elapsed % (counted ? kCountedSlots : kSlots));
  bool      on   = slot < kSlots && slot_set(pattern, slot);

  /* Edge flashes only over the counted patterns -- over 5 Hz or 10 Hz they
     would be lost anyway. Inverting rather than forcing on keeps a flash
     visible even when it lands inside one of the pattern's own flashes. */
  if (counted && now_us < g_blip_until_us)
    on = !on;

  board::led_set(board::PIN_LED_STATUS, on);
  board::led_set(board::PIN_LED_ACTIVITY, false);
}

const char *to_string(System s) {
  switch (s) {
    case System::Booting:
      return "booting";
    case System::ApMode:
      return "ap-mode";
    case System::Disconnected:
      return "disconnected";
    case System::Connected:
      return "connected";
    case System::Nominal:
      return "nominal";
    case System::Fatal:
      return "fatal";
    case System::Ota:
      return "ota";
    case System::Wiping:
      return "wiping";
  }
  return "?";
}

const char *to_string(Sensor b) {
  switch (b) {
    case Sensor::Silent:
      return "silent";
    case Sensor::Healthy:
      return "healthy";
    case Sensor::Garbage:
      return "garbage";
  }
  return "?";
}

} // namespace indicator
