/*
   flowtick-meter -- optical reader for water meters with a scanning disc,
   first for the Allmess EVK 3/110 +m. ESP-IDF firmware.

   A reflective sensor over the optical scanning disc of the +m module
   register. The disc turns in proportion to the flow and carries one bright
   and one dark sector; every bright/dark edge is a fixed volume.

   Infrastructure taken from the author's bsh-dbus-idf project -- network,
   configuration, MQTT, web interface and OTA are the same components.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "board.h"
#include "diagnostics.h"
#include "indicator.h"
#include "probation.h"
#include "recovery.h"

#include "appcfg/Config.h"
#include "appmqtt/Mqtt.h"
#include "appnet/Net.h"
#include "appweb/Web.h"
#include "edgelog/EdgeLog.h"
#include "meter/Meter.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>

static const char *const TAG = "wm";

static appnet::State g_net        = appnet::State::Idle;
static bool          g_seen_edges = false;

static void log_banner() {
  const esp_app_desc_t *app = esp_app_get_description();

  esp_chip_info_t chip;
  esp_chip_info(&chip);

  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);

  ESP_LOGI(TAG, "flowtick-meter %s (%s %s)", app->version, app->date, app->time);
  ESP_LOGI(TAG, "ESP-IDF %s, %d core(s), rev v%d.%d, %" PRIu32 " MB flash", IDF_VER, chip.cores, chip.revision / 100,
           chip.revision % 100, flash_size / (1024 * 1024));
  ESP_LOGI(TAG, "Board %s, ADC on GPIO%d", board::NAME, board::PIN_ADC);
}

static void refresh_system_led() {
  using indicator::System;
  /* A network event or the first edge must not paint over "do not unplug"
     during an update, or over the erase warning while BOOT is held. */
  if (indicator::system() == System::Ota || indicator::system() == System::Wiping)
    return;
  switch (g_net) {
    case appnet::State::ApMode:
      indicator::set_system(System::ApMode);
      break;
    case appnet::State::Connected:
      indicator::set_system(g_seen_edges ? System::Nominal : System::Connected);
      break;
    default:
      indicator::set_system(System::Disconnected);
      break;
  }
}

/* The entities Home Assistant should create. Announced once at startup and
   never again: a water meter is always the same device with the same
   entities. */
static void announce_entities() {
  appmqtt::Description dev;
  dev.device_id    = appcfg::device_id();
  dev.device_name  = appcfg::device().name;
  dev.model        = "flowtick-meter";
  dev.manufacturer = "flowtick";
  dev.firmware     = esp_app_get_description()->version;

  appmqtt::Reading readings[4] = {};

  readings[0].id           = "total";
  readings[0].name         = "Meter reading";
  readings[0].unit         = "m³";
  readings[0].device_class = "water";
  /* total_increasing, not total: the reading only ever runs forward, so Home
     Assistant may treat a drop as a meter swap instead of negative
     consumption. That is exactly what happens when the meter is replaced. */
  readings[0].state_class = "total_increasing";
  readings[0].available   = true;

  readings[1].id          = "flow";
  readings[1].name        = "Flow";
  readings[1].unit        = "L/min";
  readings[1].state_class = "measurement";
  readings[1].icon        = "mdi:water";
  readings[1].available   = true;

  /* The raw signal and the edge count are for tuning, which the web page does
     better; Home Assistant gets one status entity with the device's own
     state as attributes instead. */
  readings[2].id              = "device";
  readings[2].name            = "Status";
  readings[2].entity_category = "diagnostic";
  readings[2].icon            = "mdi:information-outline";
  readings[2].json_state      = "mode";
  /* Published every minute; three missed ones and the device is gone. */
  readings[2].expire_after_s = 180;
  readings[2].topic          = "sensor";
  readings[2].available      = true;
  /* "status" was this entity under state/ for a day, then on the root. */
  dev.retired        = {"mv", "edges", "status"};
  dev.retired_topics = {""};

  readings[3].id   = "continuous_flow";
  readings[3].name = "Continuous flow";
  readings[3].kind = "binary_sensor";
  /* "problem": Home Assistant shows it as OK / Problem, which is what it is. */
  readings[3].device_class = "problem";
  readings[3].icon         = "mdi:water-alert";
  readings[3].available    = true;

  appmqtt::announce(dev, readings, 4);
}

/* The status entity: what the device itself is doing, as one JSON object. */
static std::string status_json() {
  const meter::State m = meter::state();
  cJSON             *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "mode", meter::to_string(m.mode));
  cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
  cJSON_AddNumberToObject(o, "uptime_s", static_cast<double>(esp_timer_get_time() / 1000000));
  cJSON_AddStringToObject(o, "boot_reason", diagnostics::boot_reason());
  cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
  cJSON_AddNumberToObject(o, "min_free_heap", esp_get_minimum_free_heap_size());

  cJSON *n = cJSON_AddObjectToObject(o, "network");
  cJSON_AddStringToObject(n, "hostname", appcfg::device().name.c_str());
  cJSON_AddStringToObject(n, "ssid", appnet::ssid().c_str());
  cJSON_AddNumberToObject(n, "rssi_dbm", appnet::rssi());
  cJSON_AddNumberToObject(n, "channel", appnet::channel());
  const int tx = appcfg::tx_power_dbm();
  cJSON_AddNumberToObject(n, "tx_power_dbm", tx > 0 ? tx : appcfg::kTxPowerMaxDbm);
  uint8_t mac[6] = {};
  char    buf[24];
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(n, "mac", buf);
  }
  esp_netif_t        *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip  = {};
  if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    cJSON_AddStringToObject(n, "ip", buf);
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.netmask));
    cJSON_AddStringToObject(n, "netmask", buf);
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.gw));
    cJSON_AddStringToObject(n, "gateway", buf);
  }
  esp_netif_dns_info_t dns = {};
  if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4) {
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    cJSON_AddStringToObject(n, "dns", buf);
  }

  const appnet::Time t = appnet::time_info();
  cJSON             *c = cJSON_AddObjectToObject(o, "time");
  cJSON_AddBoolToObject(c, "synced", t.synced);
  if (!t.server.empty())
    cJSON_AddStringToObject(c, "server", t.server.c_str());
  cJSON_AddStringToObject(c, "timezone", appcfg::clock().tz_name.c_str());

  char *text = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  std::string out = text ? text : "{}";
  if (text)
    cJSON_free(text);
  return out;
}

/* Values to MQTT, every 10 s from the main loop: reading, flow and the leak
   watch. Retained, so a consumer that connects later sees them at once. The
   status goes out once a minute, so uptime and heap stay current and Home
   Assistant can tell a vanished device. */
static void publish_readings(bool include_status) {
  const meter::State s = meter::state();

  char buf[32];

  appmqtt::Reading r;
  /* Always a value. An empty retained payload would erase the reading at the
     broker whenever the signal is briefly implausible -- whether it is current
     is what the availability topic says, not the value. */
  r.available = true;

  std::snprintf(buf, sizeof(buf), "%.4f", s.total_ml / 1000000.0);
  r.id    = "total";
  r.value = buf;
  appmqtt::publish(r);

  std::snprintf(buf, sizeof(buf), "%.1f", s.flow_lmin);
  r.id    = "flow";
  r.value = buf;
  appmqtt::publish(r);

  r.id    = "continuous_flow";
  r.value = s.continuous_flow ? "On" : "Off";
  appmqtt::publish(r);

  if (include_status) {
    r.id        = "device";
    r.value     = status_json();
    r.available = true;
    appmqtt::publish(r);
  }
}

extern "C" void app_main() {
  log_banner();

  /* NVS first: diagnostics steps the transmit power down after a brownout
     and has to be able to store that. */
  if (!appcfg::begin())
    ESP_LOGE(TAG, "Configuration storage unavailable -- settings will not survive a restart");

  diagnostics::begin();
  indicator::begin();
  indicator::set_system(indicator::System::Booting);
  indicator::tick(esp_timer_get_time());

  recovery::begin();

  appmqtt::begin();

  /* Before the sampler starts, so no edge can arrive before the log is ready. */
  edgelog::begin();

  appnet::begin([](appnet::State s) {
    g_net = s;
    refresh_system_led();
    appmqtt::on_network(s == appnet::State::Connected);
  });

  /* Set before the sampler starts: it reads these without a lock. Every
     sample goes to the scope while one is open, every edge to the LED and the
     log. Both run on the sampler task, which is why neither may block. */
  meter::set_sample_sink([](int mv, bool edge, int64_t t_us) { appweb::publish_sample(mv, edge, t_us); });
  meter::set_edge_sink([](int64_t t_us) {
    indicator::note_edge();
    edgelog::record(t_us);
  });

  meter::Config mc;
  mc.adc_gpio = board::PIN_ADC;
  mc.ir_gpio  = board::PIN_IR;
  if (!meter::begin(mc)) {
    ESP_LOGE(TAG, "Cannot start the meter -- halting");
    indicator::set_system(indicator::System::Fatal);
    for (;;) {
      indicator::tick(esp_timer_get_time());
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  announce_entities();

  appweb::begin(
      []() {
        const meter::State  st = meter::state();
        const meter::Tuning tu = meter::tuning();

        appweb::Status s;
        s.total_m3        = st.total_ml / 1000000.0;
        s.flow_lmin       = st.flow_lmin;
        s.edges           = st.edges;
        s.mv              = st.mv;
        s.mv_min          = st.mv_min;
        s.mv_max          = st.mv_max;
        s.online          = st.online;
        s.mode            = meter::to_string(st.mode);
        s.rejected        = st.rejected;
        s.continuous_flow = st.continuous_flow;
        s.flow_run_s      = st.flow_run_s;
        s.dwell_ms        = tu.dwell_ms;
        s.min_edge_ms     = tu.min_edge_ms;
        s.thr_hi_mv       = tu.thr_hi_mv;
        s.thr_lo_mv       = tu.thr_lo_mv;
        s.ml_per_edge     = tu.ml_per_edge;
        s.scope           = meter::scope();
        s.net_state       = appnet::to_string(g_net);
        s.ip              = appnet::ip();
        s.ssid            = appnet::ssid();
        s.rssi            = appnet::rssi();
        s.channel         = appnet::channel();
        s.board           = board::NAME;
        s.pin_adc         = board::PIN_ADC;
        s.pin_ir          = board::PIN_IR;
        s.pin_led         = board::PIN_LED_STATUS;
        s.led_inverted    = board::LED_ACTIVE_LOW;
        return s;
      },
      [](bool active) { indicator::set_system(active ? indicator::System::Ota : indicator::System::Booting); },
      appweb::MeterApi{
          .state_json  = meter::state_json,
          .tuning_json = meter::tuning_json,
          .set_tuning  = meter::set_tuning_json,
          .set_total =
              [](double m3, std::string *err) {
                if (meter::set_total_m3(m3))
                  return true;
                if (err)
                  *err = "Reading must be between 0 and 99999 m³";
                return false;
              },
          .reset_minmax = meter::reset_minmax,
          .set_paused   = meter::set_paused,
          .set_scope =
              [](bool on) {
                meter::set_scope(on);
                appnet::set_low_latency(on); // or the stream arrives in bursts
              },
          .edges =
              [](size_t limit, const std::function<bool(const std::string &)> &emit) {
                return edgelog::for_each(limit, [&](const edgelog::Edge &e) {
                  char b[40];
                  std::snprintf(b, sizeof(b), "[%lld,%d,%u]", static_cast<long long>(e.t_ms), e.synced ? 1 : 0,
                                static_cast<unsigned>(e.boot));
                  return emit(b);
                });
              },
          .clear_edges = edgelog::clear,
      },
      appweb::DiagnosticsApi{
          .health_json    = diagnostics::health_json,
          .coredump_size  = diagnostics::coredump_size,
          .read_coredump  = diagnostics::read_coredump,
          .erase_coredump = diagnostics::erase_coredump,
      });

  probation::begin();

  refresh_system_led();

  int64_t next_publish = 0;
  int64_t next_status  = 0;
  int64_t next_health  = 0;
  int64_t next_stats   = 0;

  for (;;) {
    const int64_t now = esp_timer_get_time();

    diagnostics::feed();
    meter::tick(now);
    recovery::tick(now);

    /* Recovery and the OTA hook hand the LED back by setting Booting. Repaint
       from the network state right away instead of waiting for it to change,
       which could take forever -- the LED would stay solid on. */
    if (indicator::system() == indicator::System::Booting)
      refresh_system_led();
    indicator::tick(now);

    probation::tick(now, g_net == appnet::State::Connected);

    /* Sensor health from the detector's debounced mode, not from a single
       sample: Fault needs 0.5 s of implausible signal and clears only after
       2 s of a steady one. Resync and a deliberate pause still count as
       healthy -- the reading stays valid. Checked often, published only on
       change (and on the availability topic's own refresh). */
    if (now > next_health) {
      next_health                = now + 100000;
      const meter::State st      = meter::state();
      const bool         healthy = st.mode != meter::Mode::Fault;
      appmqtt::set_meter_online(healthy);
      indicator::set_sensor(!healthy           ? indicator::Sensor::Garbage
                            : st.flow_lmin > 0 ? indicator::Sensor::Healthy
                                               : indicator::Sensor::Silent);
    }

    if (now > next_publish) {
      next_publish = now + 10000000;

      /* Once per boot, as soon as SNTP has answered: from then on every edge
         of this boot, earlier ones included, has a wall-clock time. */
      static bool epoch_logged = false;
      if (!epoch_logged) {
        const appnet::Time t = appnet::time_info();
        if (t.synced) {
          edgelog::set_boot_epoch(t.boot_epoch_ms);
          epoch_logged = true;
        }
      }
      const bool with_status = (now > next_status);
      if (with_status)
        next_status = now + 60000000;
      publish_readings(with_status);

      const meter::State st = meter::state();
      if (!g_seen_edges && st.edges > 0) {
        g_seen_edges = true;
        refresh_system_led();
      }
    }

    if (now > next_stats) {
      next_stats            = now + 300000000;
      const meter::State st = meter::state();
      ESP_LOGI(TAG, "Reading %.4f m³, %.1f L/min, %llu edges, signal %d mV (%d..%d)", st.total_ml / 1000000.0,
               st.flow_lmin, static_cast<unsigned long long>(st.edges), st.mv, st.mv_min, st.mv_max);
      ESP_LOGI(TAG, "Network: %s%s%s", appnet::to_string(g_net), appnet::ip().empty() ? "" : " ", appnet::ip().c_str());
      ESP_LOGI(TAG, "Heap %" PRIu32 " (min %" PRIu32 "), Boot: %s", esp_get_free_heap_size(),
               esp_get_minimum_free_heap_size(), diagnostics::boot_reason());
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
