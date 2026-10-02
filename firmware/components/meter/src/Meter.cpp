/*
   See Meter.h.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "meter/Meter.h"

#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cJSON.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace meter {
namespace {

const char *const TAG = "meter";

/* Q3 = 2.5 m³/h is 41.7 L/min. At 0.5 L per edge that is just over 1.4 edges
   per second -- 100 Hz is a factor of 70 above that.

   The ADC's continuous DMA mode would be pure over-engineering here: ring
   buffers and interrupt handling for a signal that changes once a second.
   Oneshot in a task with vTaskDelayUntil is sufficient and easy to follow. */
constexpr uint32_t kSamplePeriodMs = 10;

/* Median rather than mean: it kills single outliers without smearing the
   edge. A mean would smooth both alike and so blur exactly the transition
   that matters. */
constexpr int kOversample = 8;

/* The live stream runs at 50 Hz, not the full 100: that is enough to see the
   curve move smoothly, and it halves the WebSocket load. */
constexpr uint32_t kScopeDivider = 2;

constexpr int64_t kNvsPeriodUs = 60ll * 1000 * 1000; // 1 min, and only on change

/* Outside this window something is wrong with the sensor: a loose or dead
   reflective sensor delivers a constant rail value, and daylight falling on an
   adapter that has been taken off drives it to the top. The upper limit sits
   just under the rail: the chrome sector can come close to it (measured
   3140 mV with R2 = 2.2 kOhm), but only saturation reaches it. */
constexpr int kPlausibleLoMv = 50;
constexpr int kPlausibleHiMv = 3250;

/* Implausible this long: the detector stops counting. Plausible again this
   long: it resynchronises and carries on. */
constexpr int64_t kFaultUs   = 500 * 1000;
constexpr int64_t kRecoverUs = 2000 * 1000;

/* Leak watch, see State::continuous_flow. */
constexpr int64_t kFlowGapUs    = 10ll * 60 * 1000000;
constexpr int64_t kContinuousUs = 60ll * 60 * 1000000;

adc_oneshot_unit_handle_t g_adc     = nullptr;
adc_cali_handle_t         g_cali    = nullptr;
adc_channel_t             g_chan    = ADC_CHANNEL_1;
bool                      g_cali_ok = false;

nvs_handle_t g_nvs = 0;

Tuning            g_tuning;
SampleSink        g_sink;
EdgeSink          g_edge_sink;
std::atomic<bool> g_scope{false};

/* Written by the sampler task, read from the main loop and the HTTP task.
   The reading is 64 bits and therefore not atomic on a 32-bit core -- hence
   a mutex instead of hoping it works out. */
SemaphoreHandle_t     g_mux      = nullptr;
uint64_t              g_total_ml = 0;
uint64_t              g_edges    = 0;
std::atomic<uint32_t> g_samples{0};
int                   g_mv           = 0;
int                   g_mv_min       = INT32_MAX;
int                   g_mv_max       = INT32_MIN;
bool                  g_online       = false;
Mode                  g_mode         = Mode::Resync;
uint32_t              g_rejected     = 0;
int64_t               g_run_start_us = 0;
int64_t               g_run_last_us  = INT64_MIN / 2;
std::atomic<bool>     g_paused{false};
float                 g_flow = 0.0f;

struct Guard {
  Guard() {
    xSemaphoreTake(g_mux, portMAX_DELAY);
  }
  ~Guard() {
    xSemaphoreGive(g_mux);
  }
};

// ------------------------------------------------------------------ NVS ----

void nvs_load() {
  if (nvs_open("meter", NVS_READWRITE, &g_nvs) != ESP_OK) {
    ESP_LOGE(TAG, "NVS unavailable -- nothing will survive a reboot");
    return;
  }
  uint64_t u64;
  uint32_t u32;
  if (nvs_get_u64(g_nvs, "total_ml", &u64) == ESP_OK)
    g_total_ml = u64;
  if (nvs_get_u32(g_nvs, "thr_hi", &u32) == ESP_OK)
    g_tuning.thr_hi_mv = u32;
  if (nvs_get_u32(g_nvs, "thr_lo", &u32) == ESP_OK)
    g_tuning.thr_lo_mv = u32;
  if (nvs_get_u32(g_nvs, "ml_edge", &u32) == ESP_OK)
    g_tuning.ml_per_edge = u32;
  if (nvs_get_u32(g_nvs, "dwell", &u32) == ESP_OK)
    g_tuning.dwell_ms = u32;
  if (nvs_get_u32(g_nvs, "min_edge", &u32) == ESP_OK)
    g_tuning.min_edge_ms = u32;

  ESP_LOGI(TAG, "Reading %.4f m³, thresholds %" PRIu32 "/%" PRIu32 " mV, %" PRIu32 " ml/edge", g_total_ml / 1000000.0,
           g_tuning.thr_hi_mv, g_tuning.thr_lo_mv, g_tuning.ml_per_edge);
}

void nvs_store_total() {
  static uint64_t written = UINT64_MAX;
  uint64_t        now;
  {
    Guard g;
    now = g_total_ml;
  }
  if (!g_nvs || now == written)
    return;
  nvs_set_u64(g_nvs, "total_ml", now);
  nvs_commit(g_nvs);
  written = now;
}

// ------------------------------------------------------------------ ADC ----

int cmp_int(const void *a, const void *b) {
  return *static_cast<const int *>(a) - *static_cast<const int *>(b);
}

int read_mv() {
  int buf[kOversample];
  for (int i = 0; i < kOversample; i++) {
    int raw = 0;
    adc_oneshot_read(g_adc, g_chan, &raw);
    buf[i] = raw;
  }
  qsort(buf, kOversample, sizeof(int), cmp_int);
  const int raw = (buf[kOversample / 2 - 1] + buf[kOversample / 2]) / 2;

  int mv = raw;
  if (g_cali_ok)
    adc_cali_raw_to_voltage(g_cali, raw, &mv);
  return mv;
}

/* The edge detector, one call per sample. Three layers on top of the
   hysteresis, each catching what the one before lets through:

     dwell      a crossing must last dwell_ms -- kills spikes and bounce
     rate       edges closer than min_edge_ms wait -- noise cannot run away
     fault      implausible for kFaultUs stops counting; plausible again for
                kRecoverUs resynchronises: the detector waits until the signal
                is clearly beyond one threshold for dwell_ms, takes that as the
                sector it sees and carries on without counting an edge

   Resync is also where it starts after boot: the sensor may well sit over
   chrome, and assuming "dark" there counted a phantom edge on every restart. */
struct Detector {
  bool    high       = false;
  int64_t cand_since = -1; // start of a crossing not yet confirmed
  int64_t last_edge  = INT64_MIN / 2;
  int64_t out_since  = -1; // start of an implausible stretch
  int64_t in_since   = -1; // start of a plausible stretch while in Fault

  /* Caller holds the mutex. Returns true for a counted edge. */
  bool step(int mv, int64_t now, bool plausible) {
    const Tuning &t  = g_tuning;
    const int     hi = static_cast<int>(t.thr_hi_mv), lo = static_cast<int>(t.thr_lo_mv);

    if (g_paused.load()) {
      g_mode     = Mode::Paused;
      cand_since = -1;
      return false;
    }
    if (g_mode == Mode::Paused)
      g_mode = Mode::Resync;

    // Fault in and out.
    if (!plausible) {
      in_since = -1;
      if (out_since < 0)
        out_since = now;
      if (g_mode != Mode::Fault && now - out_since >= kFaultUs) {
        g_mode = Mode::Fault;
        ESP_LOGW(TAG, "Signal implausible (%d mV) -- not counting until it is steady again", mv);
      }
    } else {
      out_since = -1;
      if (g_mode == Mode::Fault) {
        if (in_since < 0)
          in_since = now;
        if (now - in_since >= kRecoverUs)
          g_mode = Mode::Resync;
      }
    }
    if (g_mode == Mode::Fault || !plausible) {
      cand_since = -1;
      return false;
    }

    /* Not from one sample against the midpoint: an adapter still being put
       on, or a disc that stopped on the boundary, sits between the
       thresholds, and guessing a side there counted a phantom edge. Between
       them nothing is decided. A disc parked on the boundary then costs at
       most the one edge that moves it off -- missing 0.5 L once rather than
       inventing it. */
    if (g_mode == Mode::Resync) {
      const bool clear = mv > hi || mv < lo;
      if (!clear || (cand_since >= 0 && (mv > hi) != high)) {
        cand_since = clear ? now : -1;
        high       = mv > hi;
        return false;
      }
      if (cand_since < 0) {
        cand_since = now;
        high       = mv > hi;
      }
      if (now - cand_since < static_cast<int64_t>(t.dwell_ms) * 1000)
        return false;
      cand_since = -1;
      g_mode     = Mode::Run;
      ESP_LOGI(TAG, "Resynchronised on the %s sector (%d mV)", high ? "bright" : "dark", mv);
      return false;
    }

    // Run: hysteresis, confirmed by dwell, limited by rate.
    const bool beyond = high ? (mv < lo) : (mv > hi);
    if (!beyond) {
      if (cand_since >= 0)
        g_rejected++;
      cand_since = -1;
      return false;
    }
    if (cand_since < 0)
      cand_since = now;
    if (now - cand_since < static_cast<int64_t>(t.dwell_ms) * 1000)
      return false;
    if (now - last_edge < static_cast<int64_t>(t.min_edge_ms) * 1000)
      return false;
    high       = !high;
    cand_since = -1;
    last_edge  = now;
    return true;
  }
};

void sampler_task(void *) {
  Detector   det;
  TickType_t next = xTaskGetTickCount();
  uint32_t   div  = 0;
  /* An edge on a sample the stream skips is carried to the next one it
     sends, or the scope would lose the marker. */
  bool edge_pending = false;

  for (;;) {
    const int mv = read_mv();
    g_samples.fetch_add(1, std::memory_order_relaxed);
    const int64_t now  = esp_timer_get_time();
    bool          edge = false;

    {
      Guard g;
      g_mv     = mv;
      g_online = (mv > kPlausibleLoMv && mv < kPlausibleHiMv);
      if (mv < g_mv_min)
        g_mv_min = mv;
      if (mv > g_mv_max)
        g_mv_max = mv;

      edge = det.step(mv, now, g_online);
      if (edge) {
        g_total_ml += g_tuning.ml_per_edge;
        g_edges++;
        if (now - g_run_last_us >= kFlowGapUs)
          g_run_start_us = now;
        g_run_last_us = now;
      }
    }

    /* Both edges count: the bright and the dark sector each give one event,
       so twice the resolution of the original module with its 1 L. */
    if (edge && g_edge_sink)
      g_edge_sink(now);

    /* Only while the stream runs: an edge from before it was switched on
       would otherwise ride on its first sample, long after it happened. */
    const bool scope = g_scope.load();
    edge_pending     = scope && (edge_pending || edge);
    if (scope && g_sink && (++div % kScopeDivider == 0)) {
      g_sink(mv, edge_pending, now);
      edge_pending = false;
    }

    vTaskDelayUntil(&next, pdMS_TO_TICKS(kSamplePeriodMs));
  }
}

} // namespace

// --------------------------------------------------------------- public ----

bool begin(const Config &cfg) {
  g_mux = xSemaphoreCreateMutex();
  if (!g_mux)
    return false;

  nvs_load();
  /* Every deliberate restart -- OTA, the restart button, recovery -- goes
     through esp_restart(), which runs these first. Without it each update
     threw away up to a minute of counting (five, before), and Home Assistant reads a
     reading that went backwards as a meter swap. A power cut still costs up
     to kNvsPeriodUs; nothing runs then. */
  esp_register_shutdown_handler(nvs_store_total);

  if (cfg.ir_gpio >= 0) {
    gpio_config_t io = {};
    io.pin_bit_mask  = 1ULL << cfg.ir_gpio;
    io.mode          = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(static_cast<gpio_num_t>(cfg.ir_gpio), 1);
  }

  /* ADC1 on the C6: GPIO0..GPIO6 are CH0..CH6, there is no ADC2. */
  if (cfg.adc_gpio < 0 || cfg.adc_gpio > 6) {
    ESP_LOGE(TAG, "GPIO%d has no ADC1 channel -- on the C6 only GPIO0..GPIO6 work", cfg.adc_gpio);
    return false;
  }
  g_chan = static_cast<adc_channel_t>(cfg.adc_gpio);

  adc_oneshot_unit_init_cfg_t unit = {};
  unit.unit_id                     = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit, &g_adc) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot open the ADC unit");
    return false;
  }

  adc_oneshot_chan_cfg_t chan = {};
  chan.atten                  = ADC_ATTEN_DB_12;
  chan.bitwidth               = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(g_adc, g_chan, &chan) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot configure the ADC channel");
    return false;
  }

  /* The C6 belongs to the curve-fitting group, not line fitting like the
     classic ESP32. Without calibration we carry on in raw counts -- the
     thresholds are then in ADC steps instead of millivolts, which is enough
     for tuning but confusing in the interface. */
  adc_cali_curve_fitting_config_t c = {};
  c.unit_id                         = ADC_UNIT_1;
  c.chan                            = g_chan;
  c.atten                           = ADC_ATTEN_DB_12;
  c.bitwidth                        = ADC_BITWIDTH_DEFAULT;
  g_cali_ok                         = (adc_cali_create_scheme_curve_fitting(&c, &g_cali) == ESP_OK);
  if (!g_cali_ok)
    ESP_LOGW(TAG, "ADC calibration unavailable -- values are raw counts, not millivolts");

  ESP_LOGI(TAG, "ADC on GPIO%d (CH%d), IR LED %s", cfg.adc_gpio, static_cast<int>(g_chan),
           cfg.ir_gpio >= 0 ? "switched" : "tied to 3V3");

  xTaskCreate(sampler_task, "meter", 4096, nullptr, 10, nullptr);
  return true;
}

uint32_t sample_count() {
  return g_samples.load(std::memory_order_relaxed);
}

State state() {
  Guard g;
  State s;
  s.total_ml  = g_total_ml;
  s.flow_lmin = g_flow;
  s.edges     = g_edges;
  s.mv        = g_mv;
  s.mv_min    = (g_mv_min == INT32_MAX) ? 0 : g_mv_min;
  s.mv_max    = (g_mv_max == INT32_MIN) ? 0 : g_mv_max;
  s.online    = g_online;
  s.mode      = g_mode;
  s.rejected  = g_rejected;
  /* Measured from the first to the last edge, not to now: a 52-minute
     watering would otherwise count as an hour during its 10 quiet minutes. */
  if (esp_timer_get_time() - g_run_last_us < kFlowGapUs) {
    const int64_t span = g_run_last_us - g_run_start_us;
    s.flow_run_s       = static_cast<uint32_t>(span / 1000000);
    s.continuous_flow  = span >= kContinuousUs;
  }
  return s;
}

Tuning tuning() {
  Guard g;
  return g_tuning;
}

bool set_tuning(const Tuning &t) {
  /* A hysteresis whose upper threshold is not above the lower one is none at
     all -- it would flip on every sample and drive the meter up by cubic
     metres within seconds. Better to reject it than to puzzle later over
     where the consumption came from. */
  if (t.thr_hi_mv <= t.thr_lo_mv)
    return false;
  if (t.ml_per_edge == 0 || t.ml_per_edge > 100000)
    return false;
  if (t.dwell_ms < 10 || t.dwell_ms > 1000 || t.min_edge_ms > 5000)
    return false;

  {
    Guard g;
    g_tuning = t;
  }
  if (g_nvs) {
    nvs_set_u32(g_nvs, "thr_hi", t.thr_hi_mv);
    nvs_set_u32(g_nvs, "thr_lo", t.thr_lo_mv);
    nvs_set_u32(g_nvs, "ml_edge", t.ml_per_edge);
    nvs_set_u32(g_nvs, "dwell", t.dwell_ms);
    nvs_set_u32(g_nvs, "min_edge", t.min_edge_ms);
    nvs_commit(g_nvs);
  }
  ESP_LOGI(TAG, "Tuning: %" PRIu32 "/%" PRIu32 " mV, %" PRIu32 " ml/edge, dwell %" PRIu32 " ms, min gap %" PRIu32 " ms",
           t.thr_hi_mv, t.thr_lo_mv, t.ml_per_edge, t.dwell_ms, t.min_edge_ms);
  return true;
}

bool set_total_m3(double m3) {
  if (m3 < 0.0 || m3 > 99999.0)
    return false;
  {
    Guard g;
    g_total_ml = static_cast<uint64_t>(m3 * 1000000.0 + 0.5);
  }
  if (g_nvs) {
    Guard g;
    nvs_set_u64(g_nvs, "total_ml", g_total_ml);
    nvs_commit(g_nvs);
  }
  ESP_LOGI(TAG, "Reading set: %.4f m³", m3);
  return true;
}

void reset_minmax() {
  Guard g;
  g_mv_min = INT32_MAX;
  g_mv_max = INT32_MIN;
}

void set_paused(bool on) {
  if (g_paused.exchange(on) != on)
    ESP_LOGI(TAG, "Counting %s", on ? "paused" : "resumed -- resynchronising first");
}

const char *to_string(Mode m) {
  switch (m) {
    case Mode::Resync:
      return "resync";
    case Mode::Run:
      return "run";
    case Mode::Fault:
      return "fault";
    case Mode::Paused:
      return "paused";
  }
  return "?";
}

void set_scope(bool on) {
  g_scope.store(on);
}
bool scope() {
  return g_scope.load();
}

void set_sample_sink(SampleSink sink) {
  g_sink = std::move(sink);
}

void set_edge_sink(EdgeSink sink) {
  g_edge_sink = std::move(sink);
}

void tick(int64_t now_us) {
  static int64_t  next_nvs   = 0;
  static int64_t  last_us    = 0;
  static uint64_t last_edges = 0;
  static bool     first      = true;

  /* Flow over a fixed window, not from the spacing of two edges. At 0.5 L per
     edge and a dripping tap there would be minutes between two edges, and
     the instantaneous value computed from that would be a number that
     describes nothing. */
  /* Counted from edges, not from the reading: setting the reading by hand
     moves it by whole cubic metres, which as a difference would be a flow of
     hundreds of thousands of litres a minute -- the burst-pipe alarm. */
  if (now_us - last_us >= 10000000) {
    uint64_t edges;
    uint32_t ml_per_edge;
    {
      Guard g;
      edges       = g_edges;
      ml_per_edge = g_tuning.ml_per_edge;
    }
    if (first) {
      first = false;
    } else {
      const float dl   = static_cast<float>(edges - last_edges) * ml_per_edge / 1000.0f;
      const float dmin = static_cast<float>(now_us - last_us) / 60000000.0f;
      Guard       g;
      g_flow = (dmin > 0.0f) ? dl / dmin : 0.0f;
    }
    last_edges = edges;
    last_us    = now_us;
  }

  if (now_us >= next_nvs) {
    next_nvs = now_us + kNvsPeriodUs;
    nvs_store_total();
  }
}

// ---------------------------------------------------------------- JSON ----

std::string state_json() {
  const State s = state();
  char        buf[320];
  std::snprintf(buf, sizeof(buf),
                "{\"total_m3\":%.4f,\"flow_lmin\":%.1f,\"edges\":%llu,"
                "\"mv\":%d,\"mv_min\":%d,\"mv_max\":%d,\"online\":%s,\"scope\":%s,"
                "\"mode\":\"%s\",\"rejected\":%" PRIu32 ",\"continuous_flow\":%s,\"flow_run_s\":%" PRIu32 "}",
                s.total_ml / 1000000.0, s.flow_lmin, static_cast<unsigned long long>(s.edges), s.mv, s.mv_min, s.mv_max,
                s.online ? "true" : "false", g_scope.load() ? "true" : "false", to_string(s.mode), s.rejected,
                s.continuous_flow ? "true" : "false", s.flow_run_s);
  return buf;
}

std::string tuning_json() {
  const Tuning t = tuning();
  char         buf[200];
  std::snprintf(buf, sizeof(buf),
                "{\"thr_hi_mv\":%" PRIu32 ",\"thr_lo_mv\":%" PRIu32 ",\"ml_per_edge\":%" PRIu32 ",\"dwell_ms\":%" PRIu32
                ",\"min_edge_ms\":%" PRIu32 "}",
                t.thr_hi_mv, t.thr_lo_mv, t.ml_per_edge, t.dwell_ms, t.min_edge_ms);
  return buf;
}

bool set_tuning_json(const std::string &body, std::string *error) {
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    if (error)
      *error = "not valid JSON";
    return false;
  }

  Tuning t = tuning();
  if (cJSON *v = cJSON_GetObjectItem(root, "thr_hi_mv"); cJSON_IsNumber(v))
    t.thr_hi_mv = static_cast<uint32_t>(v->valuedouble);
  if (cJSON *v = cJSON_GetObjectItem(root, "thr_lo_mv"); cJSON_IsNumber(v))
    t.thr_lo_mv = static_cast<uint32_t>(v->valuedouble);
  if (cJSON *v = cJSON_GetObjectItem(root, "ml_per_edge"); cJSON_IsNumber(v))
    t.ml_per_edge = static_cast<uint32_t>(v->valuedouble);
  if (cJSON *v = cJSON_GetObjectItem(root, "dwell_ms"); cJSON_IsNumber(v))
    t.dwell_ms = static_cast<uint32_t>(v->valuedouble);
  if (cJSON *v = cJSON_GetObjectItem(root, "min_edge_ms"); cJSON_IsNumber(v))
    t.min_edge_ms = static_cast<uint32_t>(v->valuedouble);
  cJSON_Delete(root);

  if (!set_tuning(t)) {
    if (error)
      *error = "thr_hi_mv must be above thr_lo_mv, ml_per_edge 1-100000, dwell_ms 10-1000, min_edge_ms 0-5000";
    return false;
  }
  return true;
}

} // namespace meter
