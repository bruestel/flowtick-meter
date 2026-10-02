/*
   Knowing what went wrong on a device you cannot reach.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "diagnostics.h"

#include "appcfg/Config.h"

#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace diagnostics {
namespace {

const char *const TAG = "diag";

esp_reset_reason_t g_reason        = ESP_RST_UNKNOWN;
bool               g_registered    = false;
size_t             g_coredump_size = 0;
size_t             g_coredump_addr = 0;

const char *reason_text(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power on";
    case ESP_RST_EXT:
      return "external reset";
    case ESP_RST_SW:
      return "software restart";
    case ESP_RST_PANIC:
      return "panic or exception";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "other watchdog";
    case ESP_RST_DEEPSLEEP:
      return "wake from deep sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "unknown";
  }
}

} // namespace

void begin() {
  g_reason = esp_reset_reason();

  /* React rather than assume. The transmit power runs unrestricted until a
     brownout actually happens, because capping it pre-emptively costs range
     through the meter pit and would hide whether this supply is
     affected at all. One brownout is evidence; step down and say so. */
  if (g_reason == ESP_RST_BROWNOUT) {
    const int current = appcfg::tx_power_dbm();
    const int next    = (current == 0) ? 17 : (current > 8 ? current - 3 : current);
    if (next != current && appcfg::set_tx_power_dbm(next)) {
      ESP_LOGE(TAG,
               "Brownout: the supply sagged, most likely under the WiFi transmit "
               "peak. Transmit power reduced to %d dBm.",
               next);
    } else if (next != current) {
      ESP_LOGE(TAG, "Brownout: could not store the reduced transmit power (%d dBm)", next);
    } else {
      ESP_LOGE(TAG,
               "Brownout at %d dBm, already at the lowest step. The supply is "
               "probably not the WiFi peak -- check the USB supply and cable.",
               current);
    }
  } else if (crashed_last_boot())
    ESP_LOGE(TAG, "Last restart: %s", reason_text(g_reason));
  else
    ESP_LOGI(TAG, "Boot reason: %s", reason_text(g_reason));

  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size) {
    g_coredump_addr = addr;
    g_coredump_size = size;
    ESP_LOGW(TAG,
             "A core dump from an earlier crash is stored (%u bytes); "
             "download it from the web interface before erasing.",
             static_cast<unsigned>(size));
  }

  /* The task watchdog is already initialised by the IDF; adding this task means
     a main loop that stops running reboots the device instead of leaving it
     nominally alive but doing nothing. That distinction matters at a meter
     nobody looks at: an unresponsive device looks exactly like a dead one, but only
     one of them recovers by itself. */
  esp_err_t err = esp_task_wdt_add(nullptr);
  g_registered  = (err == ESP_OK);
  if (!g_registered)
    ESP_LOGW(TAG, "Could not register with the task watchdog: %s", esp_err_to_name(err));
}

void feed() {
  if (g_registered)
    esp_task_wdt_reset();
}

const char *boot_reason() {
  return reason_text(g_reason);
}

bool crashed_last_boot() {
  return g_reason == ESP_RST_PANIC || g_reason == ESP_RST_TASK_WDT || g_reason == ESP_RST_INT_WDT ||
         g_reason == ESP_RST_WDT || g_reason == ESP_RST_BROWNOUT;
}

bool has_coredump() {
  return g_coredump_size > 0;
}
size_t coredump_size() {
  return g_coredump_size;
}

bool read_coredump(size_t offset, void *buf, size_t len) {
  if (!g_coredump_size || offset + len > g_coredump_size)
    return false;
  const esp_partition_t *part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part)
    return false;
  return esp_partition_read(part, offset, buf, len) == ESP_OK;
}

bool erase_coredump() {
  if (esp_core_dump_image_erase() != ESP_OK)
    return false;
  g_coredump_size = 0;
  ESP_LOGI(TAG, "Core dump erased");
  return true;
}

std::string health_json() {
  char        buf[192];
  std::string o = "{";

  snprintf(buf, sizeof(buf), "\"boot_reason\":\"%s\",\"crashed\":%s,", reason_text(g_reason),
           crashed_last_boot() ? "true" : "false");
  o += buf;

  snprintf(buf, sizeof(buf), "\"uptime_s\":%lld,\"heap\":%u,\"heap_min\":%u,\"heap_largest\":%u,",
           static_cast<long long>(esp_timer_get_time() / 1000000), static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(esp_get_minimum_free_heap_size()),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
  o += buf;

  snprintf(buf, sizeof(buf), "\"coredump\":%u,\"watchdog\":%s,\"tx_power\":%d,\"tasks\":[",
           static_cast<unsigned>(g_coredump_size), g_registered ? "true" : "false", appcfg::tx_power_dbm());
  o += buf;

  /* Stack headroom per task. The interesting number is the minimum ever left --
     a task that once came within a few dozen bytes of its limit will eventually
     go past it, and that shows up here long before it crashes. */
  const UBaseType_t         count = uxTaskGetNumberOfTasks();
  std::vector<TaskStatus_t> tasks(count);
  const UBaseType_t         got = uxTaskGetSystemState(tasks.data(), count, nullptr);
  for (UBaseType_t i = 0; i < got; i++) {
    /* Task names are compile-time literals today, so this escaping guards
       against a future one rather than a present hazard -- but it was the only
       string in the firmware reaching JSON unescaped, and the page puts it into
       the document. */
    char   name[configMAX_TASK_NAME_LEN * 2];
    size_t w = 0;
    for (const char *c = tasks[i].pcTaskName; c && *c && w + 2 < sizeof(name); c++) {
      if (*c == '"' || *c == '\\')
        name[w++] = '\\';
      else if (static_cast<unsigned char>(*c) < 0x20)
        continue;
      name[w++] = *c;
    }
    name[w] = '\0';
    snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"stack_free\":%u,\"prio\":%u}", i ? "," : "", name,
             static_cast<unsigned>(tasks[i].usStackHighWaterMark), static_cast<unsigned>(tasks[i].uxCurrentPriority));
    o += buf;
  }

  o += "]}";
  return o;
}

} // namespace diagnostics
