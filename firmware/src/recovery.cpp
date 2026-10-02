/*
   Recovery hold on the BOOT button.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "recovery.h"
#include "board.h"
#include "indicator.h"

#include "appcfg/Config.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace recovery {
namespace {

const char *const TAG = "recovery";

/* Long enough that it cannot happen by accident -- the same button also enters
   download mode when held during reset, so a brief press must stay harmless. */
constexpr int64_t kHoldUs = 5 * 1000000LL;

/* Below this the press is ignored entirely, which absorbs both contact bounce
   and someone brushing the board while fitting it into the housing. */
constexpr int64_t kArmUs = 1 * 1000000LL;

int64_t g_pressed_since = 0;
bool    g_armed         = false;

/* Set when the button is already down as the firmware starts. Somebody holding
   it then is reaching for download mode, or the pin is stuck; either way the
   press did not begin as a request to erase anything, so it must not become
   one by being held a moment too long. Cleared by letting go. */
bool g_ignore_until_release = false;

void wipe_and_reboot() {
  /* Both, deliberately. Password protection also covers the fallback access
     point, so clearing the network alone could still leave the device
     unreachable -- it would come up as an access point demanding a password
     nobody remembers. This is the reset for getting back in, so it resets
     everything that can keep you out. The meter reading, tuning and the other
     settings are untouched. */
  ESP_LOGW(TAG, "Recovery hold: erasing WiFi credentials and password protection");
  appcfg::clear_wifi();
  appcfg::set_auth_enabled(false);
  appcfg::set_password("admin", "");

  /* Three deliberate flashes so it is visible that something happened, rather
     than leaving the user wondering whether the hold registered. */
  for (int i = 0; i < 3; i++) {
    board::led_set(board::PIN_LED_STATUS, false);
    board::led_set(board::PIN_LED_ACTIVITY, false);
    vTaskDelay(pdMS_TO_TICKS(120));
    board::led_set(board::PIN_LED_STATUS, true);
    board::led_set(board::PIN_LED_ACTIVITY, true);
    vTaskDelay(pdMS_TO_TICKS(120));
  }

  ESP_LOGW(TAG, "Rebooting into setup mode");
  esp_restart();
}

} // namespace

void begin() {
  if (board::PIN_BOOT < 0) {
    ESP_LOGI(TAG, "No BOOT button configured; recovery hold unavailable");
    return;
  }
  board::button_init(board::PIN_BOOT);

  /* If it is already down at startup the user is most likely trying to enter
     download mode, or the pin is stuck. Either way, do not arm on it. */
  if (board::button_pressed(board::PIN_BOOT)) {
    g_ignore_until_release = true;
    ESP_LOGW(TAG, "BOOT button is down at startup; ignoring until released");
  }
}

void tick(int64_t now_us) {
  if (board::PIN_BOOT < 0)
    return;

  /* A press that was already in progress at startup never counts, however long
     it lasts. Only letting go makes the button live again. */
  if (g_ignore_until_release) {
    if (!board::button_pressed(board::PIN_BOOT)) {
      g_ignore_until_release = false;
      ESP_LOGI(TAG, "BOOT button released; recovery hold armed again");
    }
    return;
  }

  if (!board::button_pressed(board::PIN_BOOT)) {
    if (g_armed) {
      ESP_LOGI(TAG, "Recovery hold released, nothing changed");
      g_armed = false;
      /* Booting hands the LED back; the main loop repaints it on its next pass. */
      indicator::set_system(indicator::System::Booting);
    }
    g_pressed_since = 0;
    return;
  }

  if (g_pressed_since == 0) {
    g_pressed_since = now_us;
    return;
  }

  const int64_t held = now_us - g_pressed_since;

  if (!g_armed && held >= kArmUs) {
    g_armed = true;
    ESP_LOGW(TAG, "Hold for %llds to erase the WiFi credentials", (kHoldUs - kArmUs) / 1000000);
    indicator::set_system(indicator::System::Wiping);
  }

  if (held >= kHoldUs)
    wipe_and_reboot();
}

} // namespace recovery
