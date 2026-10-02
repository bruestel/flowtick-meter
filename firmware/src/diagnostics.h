/*
   Knowing what went wrong on a device you cannot reach.

   Once the board sits on the meter, often in a pit or a basement, there is no
   console and no cable. If
   it reboots, the only evidence is whatever it kept: why the last restart
   happened, whether a crash was recorded, and how close the tasks are running to
   their stack limits.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace diagnostics {

/* Registers this task with the watchdog and records why the device last
   restarted. Call early, before anything can hang. */
void begin();

/* Feed the watchdog. Call from the main loop; a loop that stops running is
   exactly the condition worth rebooting for. */
void feed();

/* Reset reason of the current boot, in words. */
const char *boot_reason();
bool        crashed_last_boot();

/* True if a core dump from an earlier crash is stored in flash. */
bool has_coredump();

/* Rendered for the HTTP layer. Tasks, stack headroom, heap, reset reason. */
std::string health_json();

/* The stored core dump, for offline analysis with esp-coredump. Returns the
   number of bytes written, or 0 if there is none. */
size_t coredump_size();
bool   read_coredump(size_t offset, void *buf, size_t len);
bool   erase_coredump();

} // namespace diagnostics
