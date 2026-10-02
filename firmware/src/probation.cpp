/*
   See probation.h.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "probation.h"

#include "appcfg/Config.h"
#include "appnet/Net.h"
#include "appweb/Web.h"
#include "meter/Meter.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

namespace probation {
namespace {

const char *const TAG = "probation";

constexpr int64_t kUsPerS         = 1000000;
constexpr int64_t kUptimeUs       = 30 * kUsPerS;
constexpr int     kWindowS        = 60;  // self-test alone (curl)
constexpr int     kWindowWithUiS  = 120; // self-test and the page's confirmation
constexpr int     kSelfTestDelayS = 20;  // web server first asked after this
constexpr int     kSelfTestRetryS = 5;
/* At 100 Hz, fewer than this in a second means the sampler is stuck. */
constexpr uint32_t kMinSamplesPerS = 50;

bool              g_active       = false;
bool              g_need_ui      = false;
bool              g_have_wifi    = false; // credentials stored; read once, not every tick
int               g_window_s     = 0;
int64_t           g_deadline_us  = 0;
uint32_t          g_samples_seen = 0;
int64_t           g_samples_at   = 0;
bool              g_sampler_ok   = false;
std::atomic<bool> g_http_ok{false};

void self_test_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(kSelfTestDelayS * 1000));
  while (!appweb::self_check())
    vTaskDelay(pdMS_TO_TICKS(kSelfTestRetryS * 1000));
  g_http_ok = true;
  vTaskDelete(nullptr);
}

} // namespace

void begin() {
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  g_active = state == ESP_OTA_IMG_PENDING_VERIFY;
  if (!g_active)
    return;
  g_need_ui     = appcfg::ota_confirm_by_ui();
  g_have_wifi   = appcfg::wifi().configured();
  g_window_s    = g_need_ui ? kWindowWithUiS : kWindowS;
  g_deadline_us = esp_timer_get_time() + g_window_s * kUsPerS;
  ESP_LOGW(TAG, "New image on probation: self-test%s, %d s to pass", g_need_ui ? " and page confirmation" : "",
           g_window_s);
  xTaskCreate(self_test_task, "selftest", 3072, nullptr, 2, nullptr);
}

void tick(int64_t now_us, bool station_connected) {
  if (!g_active)
    return;

  /* Alive means still counting up a second later, not merely a count. */
  if (now_us - g_samples_at >= kUsPerS) {
    const uint32_t n = meter::sample_count();
    g_sampler_ok     = n > g_samples_seen + kMinSamplesPerS;
    g_samples_seen   = n;
    g_samples_at     = now_us;
  }

  appweb::Probation p;
  p.active      = true;
  p.window_s    = g_window_s;
  p.left_s      = static_cast<int>((g_deadline_us - now_us) / kUsPerS);
  p.uptime      = now_us >= kUptimeUs;
  p.sampler     = g_sampler_ok;
  p.network     = g_have_wifi ? station_connected : (station_connected || appnet::provisioning());
  p.web_server  = g_http_ok.load();
  p.page_needed = g_need_ui;
  p.page        = !g_need_ui || appweb::ui_confirmed();
  appweb::set_probation(p);

  if (p.uptime && p.sampler && p.network && p.web_server && p.page) {
    g_active = false;
    appcfg::set_ota_confirm_by_ui(false);
    appweb::set_probation(appweb::Probation{});
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "New image passed its self-test%s, rollback cancelled", g_need_ui ? " and was confirmed" : "");
  } else if (now_us > g_deadline_us) {
    ESP_LOGE(TAG, "New image failed (uptime %d, sampler %d, network %d, web server %d, page %d) -- rolling back",
             p.uptime, p.sampler, p.network, p.web_server, p.page);
    appcfg::set_ota_confirm_by_ui(false);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

} // namespace probation
