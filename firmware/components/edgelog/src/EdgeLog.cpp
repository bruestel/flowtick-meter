/*
   See EdgeLog.h.

   Layout: the partition is a ring of 4 KB sectors, each 512 slots of 8 bytes.
   Slot 0 of every sector is a header carrying a sequence number; the sector
   with the highest one is where writing continues. A slot is

     type (1) | payload, 48 bit little endian (6) | crc8 over the first 7 (1)

   and an erased slot reads 0xFF throughout. A slot that is neither -- power
   cut in the middle of a write -- fails its crc and is skipped by readers;
   the writer never goes back to it.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "edgelog/EdgeLog.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace edgelog {
namespace {

const char *const TAG = "edgelog";

constexpr uint8_t kPartitionSubtype = 0x40;
constexpr size_t  kSector           = 4096;
constexpr size_t  kSlot             = 8;
constexpr size_t  kSlotsPerSector   = kSector / kSlot;

enum Type : uint8_t {
  kHeader = 0x10, // payload: sector sequence number
  kBoot   = 0x20, // payload: esp_reset_reason()
  kEpoch  = 0x30, // payload: epoch ms at which this boot's uptime was zero
  kEdge   = 0x40, // payload: uptime ms
  kErased = 0xFF,
};

struct Slot {
  uint8_t b[kSlot];
};
static_assert(sizeof(Slot) == kSlot);

const esp_partition_t *g_part    = nullptr;
size_t                 g_sectors = 0;
size_t                 g_head    = 0; // sector being written
uint32_t               g_seq     = 0; // its sequence number
size_t                 g_pos     = 0; // next free slot in it; kSlotsPerSector = full

SemaphoreHandle_t   g_lock  = nullptr;
QueueHandle_t       g_queue = nullptr;
std::atomic<size_t> g_dropped{0};

/* Readers and the writer share it under g_lock. A sector at a time keeps the
   reads few without holding 256 KB. */
Slot g_buf[kSlotsPerSector];

uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++)
      c = (c & 0x80) ? static_cast<uint8_t>((c << 1) ^ 0x07) : static_cast<uint8_t>(c << 1);
  }
  return c;
}

Slot make(uint8_t type, uint64_t payload) {
  Slot s;
  s.b[0] = type;
  for (int i = 0; i < 6; i++)
    s.b[1 + i] = static_cast<uint8_t>(payload >> (8 * i));
  s.b[7] = crc8(s.b, 7);
  return s;
}

bool erased(const Slot &s) {
  for (uint8_t v : s.b)
    if (v != 0xFF)
      return false;
  return true;
}

bool valid(const Slot &s) {
  return s.b[0] != kErased && crc8(s.b, 7) == s.b[7];
}

uint64_t payload(const Slot &s) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++)
    v |= static_cast<uint64_t>(s.b[1 + i]) << (8 * i);
  return v;
}

size_t offset(size_t sector, size_t slot) {
  return sector * kSector + slot * kSlot;
}

bool read_header(size_t sector, uint32_t *seq) {
  Slot s;
  if (esp_partition_read(g_part, offset(sector, 0), &s, kSlot) != ESP_OK)
    return false;
  if (!valid(s) || s.b[0] != kHeader)
    return false;
  *seq = static_cast<uint32_t>(payload(s));
  return true;
}

/* Caller holds g_lock. */
bool start_sector(size_t sector, uint32_t seq) {
  if (esp_partition_erase_range(g_part, offset(sector, 0), kSector) != ESP_OK)
    return false;
  const Slot h = make(kHeader, seq);
  if (esp_partition_write(g_part, offset(sector, 0), &h, kSlot) != ESP_OK)
    return false;
  g_head = sector;
  g_seq  = seq;
  g_pos  = 1;
  return true;
}

/* Caller holds g_lock. */
void append(const Slot &s) {
  if (g_pos >= kSlotsPerSector && !start_sector((g_head + 1) % g_sectors, g_seq + 1)) {
    ESP_LOGE(TAG, "Could not start sector %u", static_cast<unsigned>((g_head + 1) % g_sectors));
    return;
  }
  if (esp_partition_write(g_part, offset(g_head, g_pos), &s, kSlot) != ESP_OK)
    ESP_LOGW(TAG, "Write failed at sector %u slot %u", static_cast<unsigned>(g_head), static_cast<unsigned>(g_pos));
  /* Advance even on failure: the slot may be half written, and writing into
     it again could only produce garbage. */
  g_pos++;
}

void writer(void *) {
  Slot s;
  for (;;) {
    if (xQueueReceive(g_queue, &s, portMAX_DELAY) != pdTRUE)
      continue;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    append(s);
    xSemaphoreGive(g_lock);
  }
}

void enqueue(const Slot &s) {
  if (!g_queue || xQueueSend(g_queue, &s, 0) != pdTRUE)
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

/* Sectors in writing order, oldest first. Caller holds g_lock. */
std::vector<size_t> sectors_in_order() {
  std::vector<std::pair<uint32_t, size_t>> v;
  for (size_t i = 0; i < g_sectors; i++) {
    uint32_t seq;
    if (read_header(i, &seq))
      v.emplace_back(seq, i);
  }
  std::sort(v.begin(), v.end());
  std::vector<size_t> out;
  out.reserve(v.size());
  for (const auto &p : v)
    out.push_back(p.second);
  return out;
}

/* Every valid slot, oldest first. Caller holds g_lock. */
template <typename F> void walk(const std::vector<size_t> &order, F &&fn) {
  for (size_t sector : order) {
    if (esp_partition_read(g_part, offset(sector, 0), g_buf, kSector) != ESP_OK)
      continue;
    for (size_t i = 1; i < kSlotsPerSector; i++)
      if (valid(g_buf[i]) && !fn(g_buf[i]))
        return;
  }
}

int64_t g_boot_epoch_ms = 0;

} // namespace

bool begin() {
  g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(kPartitionSubtype),
                                    "edgelog");
  if (!g_part) {
    ESP_LOGE(TAG, "No \"edgelog\" partition -- edges are not logged");
    return false;
  }
  g_sectors = g_part->size / kSector;
  g_lock    = xSemaphoreCreateMutex();

  xSemaphoreTake(g_lock, portMAX_DELAY);
  bool found = false;
  for (size_t i = 0; i < g_sectors; i++) {
    uint32_t seq;
    if (read_header(i, &seq) && (!found || seq > g_seq)) {
      found  = true;
      g_head = i;
      g_seq  = seq;
    }
  }

  if (!found) {
    ESP_LOGI(TAG, "Empty or foreign partition, formatting");
    if (!start_sector(0, 1)) {
      xSemaphoreGive(g_lock);
      ESP_LOGE(TAG, "Format failed");
      g_part = nullptr;
      return false;
    }
  } else {
    /* Continue after the last slot that is not erased -- a half-written one
       counts as used. */
    esp_partition_read(g_part, offset(g_head, 0), g_buf, kSector);
    g_pos = kSlotsPerSector;
    while (g_pos > 1 && erased(g_buf[g_pos - 1]))
      g_pos--;
  }

  append(make(kBoot, static_cast<uint64_t>(esp_reset_reason())));
  xSemaphoreGive(g_lock);

  g_queue = xQueueCreate(128, sizeof(Slot));
  xTaskCreate(writer, "edgelog", 3072, nullptr, 2, nullptr);

  ESP_LOGI(TAG, "%u sectors, %u edges capacity, writing sector %u slot %u (seq %u)", static_cast<unsigned>(g_sectors),
           static_cast<unsigned>(g_sectors * (kSlotsPerSector - 1)), static_cast<unsigned>(g_head),
           static_cast<unsigned>(g_pos), static_cast<unsigned>(g_seq));
  return true;
}

void record(int64_t uptime_us) {
  if (g_part)
    enqueue(make(kEdge, static_cast<uint64_t>(uptime_us / 1000)));
}

void set_boot_epoch(int64_t boot_epoch_ms) {
  g_boot_epoch_ms = boot_epoch_ms;
  if (g_part)
    enqueue(make(kEpoch, static_cast<uint64_t>(boot_epoch_ms)));
}

size_t for_each(size_t limit, const std::function<bool(const Edge &)> &fn) {
  if (!g_part)
    return 0;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const std::vector<size_t> order = sectors_in_order();

  /* Pass one: how many edges, and the wall clock of each boot. The epoch of a
     boot can arrive after its first edges, so it has to be known before any
     of them is emitted. Boot 0 is whatever precedes the oldest surviving boot
     marker; its epoch is lost with the sector the ring overwrote. */
  std::vector<int64_t> epochs{-1};
  size_t               total = 0;
  walk(order, [&](const Slot &s) {
    if (s.b[0] == kBoot)
      epochs.push_back(-1);
    else if (s.b[0] == kEpoch)
      epochs.back() = static_cast<int64_t>(payload(s));
    else if (s.b[0] == kEdge)
      total++;
    return true;
  });

  size_t   skip = (limit && total > limit) ? total - limit : 0;
  uint32_t boot = 0;
  walk(order, [&](const Slot &s) {
    if (s.b[0] == kBoot) {
      boot++;
      return true;
    }
    if (s.b[0] != kEdge)
      return true;
    if (skip) {
      skip--;
      return true;
    }
    const int64_t up   = static_cast<int64_t>(payload(s));
    const int64_t base = epochs[boot];
    return fn(Edge{base >= 0 ? base + up : up, base >= 0, boot});
  });
  xSemaphoreGive(g_lock);
  return total;
}

bool clear() {
  if (!g_part)
    return false;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  bool ok = esp_partition_erase_range(g_part, 0, g_sectors * kSector) == ESP_OK && start_sector(0, 1);
  if (ok) {
    /* Keep what this boot already knows, or its later edges lose their time. */
    append(make(kBoot, static_cast<uint64_t>(esp_reset_reason())));
    if (g_boot_epoch_ms > 0)
      append(make(kEpoch, static_cast<uint64_t>(g_boot_epoch_ms)));
  }
  xSemaphoreGive(g_lock);
  ESP_LOGW(TAG, "Log cleared%s", ok ? "" : " -- FAILED");
  return ok;
}

Stats stats() {
  Stats s;
  s.capacity = g_sectors * (kSlotsPerSector - 1);
  s.dropped  = g_dropped.load(std::memory_order_relaxed);
  return s;
}

} // namespace edgelog
