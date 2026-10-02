/*
   Edge log: when every edge happened, kept across power cuts.

   An append-only ring in its own flash partition ("edgelog", see
   partitions.csv). Why this does not wear the flash out, where NVS would:

     NOR flash wears by erasing, not by writing. Writing only clears bits, so
     an entry can go straight into bytes that are still erased, without
     touching anything else. This log never rewrites an entry; it erases a
     4 KB sector only once the ring comes round to it again, i.e. once per
     ~500 edges per sector. With 64 sectors and 100,000 erase cycles each, the
     partition lasts for some 3 billion edges -- over a thousand years at a
     household's rate. NVS, by contrast, rewrites its pages as values change,
     which is why the meter reading goes there only every five minutes.

   Entries carry the uptime, not the wall clock: an edge can come before SNTP
   has answered, or without any network at all. The wall clock is logged once
   per boot as soon as it is known, and a reader resolves every entry of that
   boot through it -- including the ones written before it arrived.

   Writing happens on a task of its own: an erase takes tens of milliseconds,
   and the sampler must never wait for one.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace edgelog {

/* Finds the write position and logs a boot marker. False if the partition is
   missing, in which case every other call is a harmless no-op. */
bool begin();

/* One edge, at the given uptime. Never blocks: if the writer falls behind,
   the edge is dropped and counted rather than stalling the caller. */
void record(int64_t uptime_us);

/* The wall-clock time at which this boot's uptime read zero. Call once the
   clock is synced; calling again (a correction) is fine, the last one wins. */
void set_boot_epoch(int64_t boot_epoch_ms);

struct Edge {
  int64_t  t_ms; // epoch ms if synced, else uptime ms of that boot
  bool     synced;
  uint32_t boot; // counts boots within the log, oldest first
};

/* Oldest first, the newest `limit` edges (0 = all). The callback returns
   false to stop. Returns the number of edges in the log. */
size_t for_each(size_t limit, const std::function<bool(const Edge &)> &fn);

/* Erase everything. */
bool clear();

struct Stats {
  size_t capacity = 0; // entries the ring can hold
  size_t dropped  = 0; // edges lost because the writer fell behind
};
Stats stats();

} // namespace edgelog
