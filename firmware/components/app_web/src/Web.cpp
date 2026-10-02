/*
   HTTP server: REST API, WebSocket live scope, embedded UI.

   Infrastructure taken from the author's bsh-dbus-idf project: HTTP, auth,
   OTA and WebSocket are unchanged; the API surface is the meter's own.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "appweb/Web.h"
#include "appcfg/Config.h"
#include "appmqtt/Mqtt.h"
#include "appnet/Net.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>

#include <cJSON.h>

#include <lwip/sockets.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

/* Generated from www/index.html at configure time, see CMakeLists.txt. */
extern "C" {
extern const unsigned char ui_index_html_gz[];
extern const unsigned int  ui_index_html_gz_len;
}

namespace appweb {
namespace {

const char *const TAG = "appweb";

httpd_handle_t g_server = nullptr;
StatusProvider g_status;
OtaHook        g_ota_hook;
MeterApi       g_meter;
DiagnosticsApi g_diag;

/* Lets the application drive the LEDs during an update without this component
   knowing anything about them. */
void indicator_hook(bool active) {
  if (g_ota_hook)
    g_ota_hook(active);
}

/* Enough to read back the server's socket table; must match max_open_sockets
   below. Monitors are no longer counted separately -- see the websocket
   section for why the local client table is gone. */
constexpr size_t kMaxSockets = 7;
constexpr int    kQueueLen   = 64;

/* One serialised sample, ready to send. Fixed size so a burst does not
   fragment the heap. {"t":"s","s":N,"ts":N,"v":N,"e":0} stays well below it
   even with large sequence numbers. */
constexpr size_t kMsgText = 80;
struct Msg {
  char text[kMsgText];
};

QueueHandle_t g_queue = nullptr;
/* Incremented by the application task when a message will not fit the queue and
   read-and-cleared by the pump task that reports it. A lost increment costs a
   drop notice, not correctness -- but an atomic costs nothing here. */
std::atomic<uint32_t> g_dropped{0};

/* Set by POST /api/v1/ota/confirm: the update page is back and sees the new
   image working. */
std::atomic<bool> g_ui_confirmed{false};
/* The trial period of a new image, from probation.cpp. */
std::mutex g_probation_mx;
Probation  g_probation;

/* Monotonic id stamped onto every sample message as "s". Lets a scope say what
   it last saw so a reconnect can be filled from the ring below instead of
   resuming blind. Only ever incremented, from the sampler in publish_sample. */
uint32_t g_seq = 0;

/* A ring of the most recent serialised messages, kept so a monitor that
   connects or reconnects can be handed what it missed before the live stream
   resumes. The WebSocket stays the only delivery path; this is just its memory.
   Written from the app task (publish_*) and read from the httpd task (replay in
   ws_handler), so every touch is under g_ring_mux. */
constexpr size_t kRingCap = 512;
struct RingItem {
  uint32_t seq;
  char     text[kMsgText];
};
/* Allocated the first time somebody asks to see the traffic, not at startup.
   At 512 entries this is 74 KiB, and it was the largest thing this firmware
   owned -- reserved for good on a device that spends its life at a meter with
   nobody looking at the scope. No reason to reserve memory for a window that
   is closed most of the time. */
RingItem         *g_ring       = nullptr;
size_t            g_ring_head  = 0; // where the next append goes
size_t            g_ring_count = 0; // valid entries, <= kRingCap
SemaphoreHandle_t g_ring_mux   = nullptr;

/* Append one already-serialised message to the ring. Cheap: a copy under the
   mutex, no allocation. */
/* Called when a monitor appears, from the HTTP task. Failure is not fatal: the
   monitor then shows what arrives from now on, without the replay of what it
   missed, which is a smaller loss than refusing to open. */
bool ring_reserve() {
  if (!g_ring_mux)
    return false;
  xSemaphoreTake(g_ring_mux, portMAX_DELAY);
  if (!g_ring) {
    g_ring = static_cast<RingItem *>(calloc(kRingCap, sizeof(RingItem)));
    if (g_ring)
      ESP_LOGI(TAG, "Monitor ring: %u bytes", static_cast<unsigned>(kRingCap * sizeof(RingItem)));
    else
      ESP_LOGW(TAG, "No room for the monitor ring; replay unavailable");
  }
  const bool ok = g_ring != nullptr;
  xSemaphoreGive(g_ring_mux);
  return ok;
}

void ring_append(uint32_t seq, const char *text) {
  if (!g_ring_mux)
    return;
  xSemaphoreTake(g_ring_mux, portMAX_DELAY);
  if (!g_ring) {
    /* Nobody has ever opened the monitor, so there is nothing to replay to. */
    xSemaphoreGive(g_ring_mux);
    return;
  }
  RingItem &slot = g_ring[g_ring_head];
  slot.seq       = seq;
  std::snprintf(slot.text, sizeof(slot.text), "%s", text);
  g_ring_head = (g_ring_head + 1) % kRingCap;
  if (g_ring_count < kRingCap)
    g_ring_count++;
  xSemaphoreGive(g_ring_mux);
}

// ---------------------------------------------------------------- auth ----

/* Protection applies everywhere, including the fallback access point.

   It was exempt there at first, reasoning that a password would lock the user
   out of a device they cannot reach any other way. That reasoning does not
   survive scrutiny: jamming the WiFi is enough to force the device into
   access-point mode, and an open setup network then hands over every setting to
   whoever is nearby.

   The lockout it was meant to prevent is covered properly now -- holding the
   BOOT button for five seconds erases the WiFi credentials *and* the password,
   which needs physical access and nothing else. */
bool auth_required() {
  return appcfg::auth().enabled;
}

/* Verifying a password costs 20 000 rounds of PBKDF2, which is the right price
   for a login and the wrong one for a request. Basic authentication sends the
   header on every single request, and the interface polls several times a
   minute: without this the device would spend most of its httpd task deriving
   the same key over and over, and anyone could make it do that by sending
   requests.

   So a header that has already been verified is remembered by its hash for a
   few minutes. Only credentials that passed can ever be in here, so the cache
   cannot admit anyone; it only postpones noticing a password change, which is
   why changing one clears it. */
constexpr int64_t kAuthCacheUs    = 300 * 1000000LL;
uint8_t           g_auth_hash[32] = {};
bool              g_auth_valid    = false;
int64_t           g_auth_until    = 0;

/* A wrong password is cheap for the sender and expensive for us, so after a
   handful of failures the key derivation is skipped entirely for a while. The
   window is short enough to be invisible to somebody mistyping a password and
   long enough to make guessing pointless. */
constexpr int     kMaxAuthFailures     = 5;
constexpr int64_t kAuthBackoffUs       = 10 * 1000000LL;
int               g_auth_failures      = 0;
int64_t           g_auth_blocked_until = 0;

/* Same hash primitive the password derivation uses, through the same API, so
   this file does not pull in a second crypto interface. A failure leaves the
   digest zeroed, which simply misses the cache and costs a derivation. */
void hash_header(const std::string &header, uint8_t out[32]) {
  std::memset(out, 0, 32);
  size_t written = 0;
  psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(header.data()), header.size(), out, 32, &written);
}

void forget_auth_cache() {
  g_auth_valid         = false;
  g_auth_failures      = 0;
  g_auth_blocked_until = 0;
}

bool check_auth(httpd_req_t *req) {
  if (!auth_required())
    return true;

  size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len == 0 || len > 256)
    return false;

  std::string header(len + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, "Authorization", header.data(), len + 1) != ESP_OK)
    return false;
  header.resize(len);

  const int64_t now = esp_timer_get_time();
  uint8_t       digest[32];
  hash_header(header, digest);
  if (g_auth_valid && now < g_auth_until && std::memcmp(digest, g_auth_hash, sizeof(digest)) == 0)
    return true;

  if (g_auth_failures >= kMaxAuthFailures && now < g_auth_blocked_until)
    return false;

  const std::string prefix = "Basic ";
  if (header.rfind(prefix, 0) != 0)
    return false;

  const std::string b64 = header.substr(prefix.size());
  unsigned char     decoded[256];
  size_t            decoded_len = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                            reinterpret_cast<const unsigned char *>(b64.data()), b64.size()) != 0)
    return false;
  decoded[decoded_len] = '\0';

  std::string  creds(reinterpret_cast<char *>(decoded), decoded_len);
  const size_t colon = creds.find(':');
  if (colon == std::string::npos)
    return false;

  if (!appcfg::verify_password(creds.substr(0, colon), creds.substr(colon + 1))) {
    if (++g_auth_failures >= kMaxAuthFailures)
      g_auth_blocked_until = now + kAuthBackoffUs;
    return false;
  }

  std::memcpy(g_auth_hash, digest, sizeof(g_auth_hash));
  g_auth_valid    = true;
  g_auth_until    = now + kAuthCacheUs;
  g_auth_failures = 0;
  return true;
}

esp_err_t deny(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"flowtick\"");
  httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
  return ESP_OK;
}

esp_err_t send_json(httpd_req_t *req, cJSON *root) {
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, text);
  cJSON_free(text);
  return ESP_OK;
}

/* Reads a bounded request body. Anything larger is a mistake or an attack, not
   a configuration. */
bool read_body(httpd_req_t *req, std::string &out, size_t limit = 1024) {
  if (req->content_len == 0 || req->content_len > limit)
    return false;
  out.resize(req->content_len);
  size_t got = 0;
  while (got < req->content_len) {
    int r = httpd_req_recv(req, out.data() + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (r <= 0)
      return false;
    got += r;
  }
  return true;
}

/* One query parameter by name, bounded. Absent or oversized reads as empty,
   which every caller treats as "not given". */
std::string query_param(httpd_req_t *req, const char *key) {
  const size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 160)
    return {};
  std::string query(qlen + 1, '\0');
  if (httpd_req_get_url_query_str(req, query.data(), qlen + 1) != ESP_OK)
    return {};
  char value[80] = "";
  if (httpd_query_key_value(query.c_str(), key, value, sizeof(value)) != ESP_OK)
    return {};

  /* httpd_query_key_value hands back the raw slice, percent escapes and all.
     A browser encodes reserved characters (a colon becomes "%3A"), so without
     this a value containing one arrives escaped and matches nothing. */
  std::string out;
  for (size_t i = 0; value[i]; i++) {
    if (value[i] == '+') {
      out += ' ';
    } else if (value[i] == '%' && value[i + 1] && value[i + 2]) {
      const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(value[i + 1]), lo = nib(value[i + 2]);
      if (hi < 0 || lo < 0) {
        out += value[i]; /* Not an escape after all; take it literally. */
        continue;
      }
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += value[i];
    }
  }
  return out;
}

// --------------------------------------------------------------- OTA ----

/* Broadcast so the browser can show progress from the flash side, not just its
   own upload side -- the two diverge once the socket buffer is full. */
void report_ota(size_t written, size_t total) {
  if (!g_queue)
    return;
  Msg msg;
  snprintf(msg.text, sizeof(msg.text), "{\"t\":\"ota\",\"w\":%u,\"n\":%u}", static_cast<unsigned>(written),
           static_cast<unsigned>(total));
  xQueueSend(g_queue, &msg, 0);
}

esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t handle, const char *reason) {
  if (handle)
    esp_ota_abort(handle);
  ESP_LOGE(TAG, "OTA rejected: %s", reason);
  httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", reason);
  return send_json(req, r);
}

/* The project was called "water-meter" before it became "flowtick-meter".
   Both count as this project, so a device on either name takes an update
   built under the other -- the rename needed no cable, and going back does not
   either. */
bool same_project(const char *incoming, const char *self) {
  constexpr size_t n = sizeof(esp_app_desc_t::project_name);
  for (const char *name : {self, "flowtick-meter", "water-meter"})
    if (std::strncmp(incoming, name, n) == 0)
      return true;
  return false;
}

esp_err_t post_ota(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (!target)
    return ota_fail(req, 0, "no free OTA slot");

  const size_t total = req->content_len;
  if (total < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
    return ota_fail(req, 0, "not a firmware image (too small)");
  if (total > target->size)
    return ota_fail(req, 0, "image larger than the OTA slot");

  esp_ota_handle_t handle = 0;
  esp_err_t        err    = esp_ota_begin(target, total, &handle);
  if (err != ESP_OK)
    return ota_fail(req, 0, esp_err_to_name(err));

  ESP_LOGW(TAG, "OTA started: %u bytes into %s", static_cast<unsigned>(total), target->label);
  indicator_hook(true);

  char   buf[2048];
  size_t written = 0;
  bool   checked = false;

  /* The image header, the first segment header and the application descriptor,
     which is as far as we have to read to know whose firmware this is. */
  constexpr size_t     kDescAt       = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  constexpr size_t     kHeaderNeeded = kDescAt + sizeof(esp_app_desc_t);
  std::vector<uint8_t> head;
  head.reserve(kHeaderNeeded);

  while (written < total) {
    int got = httpd_req_recv(req, buf, std::min(sizeof(buf), total - written));
    if (got == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (got <= 0) {
      indicator_hook(false);
      return ota_fail(req, handle, "upload interrupted");
    }

    const char *data = buf;
    size_t      len  = static_cast<size_t>(got);

    /* Nothing reaches the flash before this passes. The magic byte catches
       anything that is not an ESP image at all; the project name catches
       firmware for a different device, which would otherwise brick this one in
       a way only a cable can undo.

       The header is collected across however many reads it takes rather than
       expected in the first one. A slow uplink delivers a short first chunk,
       and this used to take that as permission to skip the project name and
       write the rest unchecked. */
    if (!checked) {
      const size_t take = std::min(kHeaderNeeded - head.size(), len);
      head.insert(head.end(), buf, buf + take);
      data += take;
      len -= take;

      if (!head.empty() && head[0] != ESP_IMAGE_HEADER_MAGIC) {
        indicator_hook(false);
        return ota_fail(req, handle, "not an ESP firmware image");
      }
      if (head.size() < kHeaderNeeded)
        continue; // keep reading; still nothing written

      const auto           *desc = reinterpret_cast<const esp_app_desc_t *>(head.data() + kDescAt);
      const esp_app_desc_t *self = esp_app_get_description();
      if (!same_project(desc->project_name, self->project_name)) {
        indicator_hook(false);
        return ota_fail(req, handle, "firmware is for a different project");
      }
      ESP_LOGI(TAG, "Incoming version: %s", desc->version);
      checked = true;

      err = esp_ota_write(handle, head.data(), head.size());
      if (err != ESP_OK) {
        indicator_hook(false);
        return ota_fail(req, handle, esp_err_to_name(err));
      }
      written += head.size();
      report_ota(written, total);
      if (len == 0)
        continue;
    }

    err = esp_ota_write(handle, data, len);
    if (err != ESP_OK) {
      indicator_hook(false);
      return ota_fail(req, handle, esp_err_to_name(err));
    }
    written += len;
    report_ota(written, total);
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    indicator_hook(false);
    return ota_fail(req, 0, err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation" : esp_err_to_name(err));
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    indicator_hook(false);
    return ota_fail(req, 0, esp_err_to_name(err));
  }

  /* Uploaded from the web interface, the page will confirm the new image
     itself once it is back; from curl nobody will, and the self-test decides
     alone. The page says which with this header. */
  char       hdr[8] = {};
  const bool by_ui =
      httpd_req_get_hdr_value_str(req, "X-Confirm", hdr, sizeof(hdr)) == ESP_OK && std::strcmp(hdr, "ui") == 0;
  appcfg::set_ota_confirm_by_ui(by_ui);

  ESP_LOGW(TAG, "OTA complete, rebooting into %s", target->label);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddNumberToObject(r, "written", written);
  send_json(req, r);

  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
      },
      "ota_reboot", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

esp_err_t send_raw_json(httpd_req_t *req, const std::string &body);

/* Answered for the device itself only (see self_check), so it needs no
   login and gives nothing away: from anywhere else it does not exist. */
esp_err_t get_selftest(httpd_req_t *req) {
  sockaddr_in6 peer{};
  socklen_t    len   = sizeof(peer);
  bool         local = false;
  if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&peer), &len) == 0) {
    if (peer.sin6_family == AF_INET)
      local = reinterpret_cast<sockaddr_in *>(&peer)->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    else if (peer.sin6_family == AF_INET6) // IPv4-mapped ::ffff:127.0.0.1
      local = peer.sin6_addr.un.u32_addr[3] == htonl(INADDR_LOOPBACK) && peer.sin6_addr.un.u32_addr[2] == htonl(0xffff);
  }
  if (!local)
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, nullptr);
  return send_raw_json(req, "{\"ok\":true}");
}

/* The update page, back after the restart, confirms that it sees the new
   image working. Only half the decision: the image also has to pass its own
   self-test (probation.cpp) before it is kept. */
esp_err_t post_ota_confirm(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  g_ui_confirmed = true;
  return send_raw_json(req, "{\"ok\":true}");
}

esp_err_t get_ota_status(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t   state   = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  const esp_app_desc_t *self = esp_app_get_description();

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "partition", running->label);
  cJSON_AddStringToObject(r, "version", self->version);
  cJSON_AddStringToObject(r, "built", self->date);
  cJSON_AddBoolToObject(r, "pending_verify", state == ESP_OTA_IMG_PENDING_VERIFY);
  cJSON_AddBoolToObject(r, "ui_confirmed", g_ui_confirmed.load());
  Probation p;
  {
    std::lock_guard<std::mutex> l(g_probation_mx);
    p = g_probation;
  }
  if (p.active) {
    cJSON *o = cJSON_AddObjectToObject(r, "probation");
    cJSON_AddBoolToObject(o, "uptime", p.uptime);
    cJSON_AddBoolToObject(o, "sampler", p.sampler);
    cJSON_AddBoolToObject(o, "network", p.network);
    cJSON_AddBoolToObject(o, "web_server", p.web_server);
    cJSON_AddBoolToObject(o, "page_needed", p.page_needed);
    cJSON_AddBoolToObject(o, "page", p.page);
    cJSON_AddNumberToObject(o, "left_s", p.left_s);
    cJSON_AddNumberToObject(o, "window_s", p.window_s);
  }
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  cJSON_AddNumberToObject(r, "slot_size", next ? next->size : 0);
  return send_json(req, r);
}

// ------------------------------------------------------------ handlers ----

esp_err_t get_index(httpd_req_t *req) {
  /* The page itself carries no data -- everything comes from /api/v1/info,
     which is protected. Gating it anyway means the browser asks for credentials
     when the page is opened, so every later request already carries them. Left
     open, the page would load and then fail with 401 in the background, which
     browsers do not turn into a login prompt. */
  if (!check_auth(req))
    return deny(req);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, reinterpret_cast<const char *>(ui_index_html_gz), ui_index_html_gz_len);
}

esp_err_t get_info(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  Status s    = g_status ? g_status() : Status{};
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device", appcfg::device().name.c_str());
  cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
  cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
  cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());

  cJSON *m = cJSON_AddObjectToObject(root, "meter");
  cJSON_AddNumberToObject(m, "total_m3", s.total_m3);
  cJSON_AddNumberToObject(m, "flow_lmin", s.flow_lmin);
  cJSON_AddNumberToObject(m, "edges", static_cast<double>(s.edges));
  cJSON_AddNumberToObject(m, "mv", s.mv);
  cJSON_AddNumberToObject(m, "mv_min", s.mv_min);
  cJSON_AddNumberToObject(m, "mv_max", s.mv_max);
  cJSON_AddNumberToObject(m, "thr_hi_mv", s.thr_hi_mv);
  cJSON_AddNumberToObject(m, "thr_lo_mv", s.thr_lo_mv);
  cJSON_AddNumberToObject(m, "ml_per_edge", s.ml_per_edge);
  cJSON_AddBoolToObject(m, "scope", s.scope);
  cJSON_AddBoolToObject(m, "online", s.online);
  cJSON_AddStringToObject(m, "mode", s.mode.c_str());
  cJSON_AddNumberToObject(m, "rejected", s.rejected);
  cJSON_AddBoolToObject(m, "continuous_flow", s.continuous_flow);
  cJSON_AddNumberToObject(m, "flow_run_s", s.flow_run_s);
  cJSON_AddNumberToObject(m, "dwell_ms", s.dwell_ms);
  cJSON_AddNumberToObject(m, "min_edge_ms", s.min_edge_ms);

  /* What this firmware is running on: board, chip and pins. It turns a class
     of wiring or radio problem from a morning's detective work into a request. */
  cJSON *hw = cJSON_AddObjectToObject(root, "hardware");
  cJSON_AddStringToObject(hw, "board", s.board.c_str());
  cJSON_AddStringToObject(hw, "chip", CONFIG_IDF_TARGET);
  cJSON_AddNumberToObject(hw, "adc_pin", s.pin_adc);
  cJSON_AddNumberToObject(hw, "ir_pin", s.pin_ir);
  cJSON_AddNumberToObject(hw, "led_pin", s.pin_led);
  cJSON_AddBoolToObject(hw, "led_inverted", s.led_inverted);

  /* 0 means maximum. A brownout can lower the value by itself, which is why
     the interface shows it rather than remembering the last one it set. */
  cJSON *sys = cJSON_AddObjectToObject(root, "system");
  cJSON_AddNumberToObject(sys, "tx_power_dbm", appcfg::tx_power_dbm());
  cJSON_AddNumberToObject(sys, "tx_power_max_dbm", appcfg::kTxPowerMaxDbm);
  const appcfg::Clock ck = appcfg::clock();
  cJSON_AddStringToObject(sys, "ntp_server", ck.server.c_str());
  cJSON_AddStringToObject(sys, "tz_name", ck.tz_name.c_str());

  cJSON *net = cJSON_AddObjectToObject(root, "net");
  cJSON_AddStringToObject(net, "state", s.net_state.c_str());
  cJSON_AddNumberToObject(net, "channel", s.channel);
  cJSON_AddStringToObject(net, "ip", s.ip.c_str());
  cJSON_AddStringToObject(net, "ssid", s.ssid.c_str());
  cJSON_AddNumberToObject(net, "rssi", s.rssi);
  cJSON_AddBoolToObject(net, "provisioning", appnet::provisioning());
  const appnet::Test test = appnet::test_status();
  cJSON             *tj   = cJSON_AddObjectToObject(net, "test");
  cJSON_AddStringToObject(tj, "state", appnet::to_string(test.state));
  cJSON_AddStringToObject(tj, "ssid", test.ssid.c_str());
  cJSON_AddStringToObject(tj, "error", test.error.c_str());
  cJSON_AddStringToObject(tj, "ip", test.ip.c_str());
  cJSON_AddNumberToObject(tj, "restart_in_s", static_cast<double>(test.restart_in_s));

  /* boot_epoch_ms is the useful one: samples carry the uptime clock, so adding
     it to that stamp gives the absolute time a sample was taken -- without a
     timestamp having to travel with every single sample. */
  const appnet::Time t     = appnet::time_info();
  cJSON             *clock = cJSON_AddObjectToObject(root, "time");
  cJSON_AddBoolToObject(clock, "synced", t.synced);
  cJSON_AddStringToObject(clock, "tz", appcfg::clock().tz_name.c_str());
  if (!t.server.empty())
    cJSON_AddStringToObject(clock, "server", t.server.c_str());
  if (t.synced) {
    cJSON_AddNumberToObject(clock, "epoch_ms", static_cast<double>(t.epoch_ms));
    cJSON_AddNumberToObject(clock, "boot_epoch_ms", static_cast<double>(t.boot_epoch_ms));
  }

  cJSON *mq = cJSON_AddObjectToObject(root, "mqtt");
  cJSON_AddBoolToObject(mq, "enabled", appmqtt::enabled());
  cJSON_AddBoolToObject(mq, "connected", appmqtt::connected());

  cJSON       *auth = cJSON_AddObjectToObject(root, "auth");
  appcfg::Auth a    = appcfg::auth();
  cJSON_AddBoolToObject(auth, "enabled", a.enabled);
  cJSON_AddBoolToObject(auth, "has_password", a.has_password());
  cJSON_AddStringToObject(auth, "user", a.user.c_str());

  return send_json(req, root);
}

/* {"networks":[{"ssid":…,"rssi":…,"channel":…,"secure":…},…]}, strongest
   first. Blocks the server for the two seconds a scan takes; the setup page
   is the only thing that asks. */
esp_err_t get_wifi_scan(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::vector<appnet::Network> nets;
  if (!appnet::scan(nets)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return send_raw_json(req, "{\"error\":\"radio busy, try again in a moment\"}");
  }
  cJSON *root = cJSON_CreateObject();
  cJSON *arr  = cJSON_AddArrayToObject(root, "networks");
  for (const auto &n : nets) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", n.ssid.c_str());
    cJSON_AddNumberToObject(o, "rssi", n.rssi);
    cJSON_AddNumberToObject(o, "channel", n.channel);
    cJSON_AddBoolToObject(o, "secure", n.secure);
    cJSON_AddItemToArray(arr, o);
  }
  return send_json(req, root);
}

/* Join a network. The credentials are tried first and stored only once they
   have worked; the result appears in /info under net.test -- see
   appnet::test_credentials. Nothing is ever stored untested. */
esp_err_t post_wifi_connect(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  cJSON      *root = read_body(req, body) ? cJSON_Parse(body.c_str()) : nullptr;
  cJSON      *ssid = root ? cJSON_GetObjectItem(root, "ssid") : nullptr;
  cJSON      *pass = root ? cJSON_GetObjectItem(root, "password") : nullptr;
  if (!cJSON_IsString(ssid)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"ssid required\"}");
  }
  const appnet::TestStart t =
      appnet::test_credentials(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
  cJSON_Delete(root);
  switch (t) {
    case appnet::TestStart::Started:
      return send_raw_json(req, "{\"ok\":true,\"testing\":true}");
    case appnet::TestStart::Invalid: {
      char msg[96];
      std::snprintf(msg, sizeof(msg), "{\"error\":\"SSID (1-%u characters) or password (up to %u) invalid\"}",
                    static_cast<unsigned>(appnet::kSsidMaxLen), static_cast<unsigned>(appnet::kPasswordMaxLen));
      httpd_resp_set_status(req, "400 Bad Request");
      return send_raw_json(req, msg);
    }
    case appnet::TestStart::Settling:
      httpd_resp_set_status(req, "409 Conflict");
      return send_raw_json(req, "{\"error\":\"the last network change is still being applied; try again shortly\"}");
    case appnet::TestStart::Offline:
      httpd_resp_set_status(req, "409 Conflict");
      return send_raw_json(req, "{\"error\":\"not connected\"}");
    case appnet::TestStart::Busy:
      httpd_resp_set_status(req, "409 Conflict");
      return send_raw_json(req, "{\"error\":\"a connection attempt is already running\"}");
  }
  return ESP_FAIL; // not reached: every TestStart is handled above
}

esp_err_t post_auth(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    return ESP_OK;
  }

  cJSON *pw      = cJSON_GetObjectItem(root, "password");
  cJSON *user    = cJSON_GetObjectItem(root, "user");
  cJSON *enabled = cJSON_GetObjectItem(root, "enabled");

  const char *error = nullptr;
  if (cJSON_IsString(pw))
    if (!appcfg::set_password(cJSON_IsString(user) ? user->valuestring : "admin", pw->valuestring))
      error = "could not store password";

  if (!error && cJSON_IsBool(enabled))
    if (!appcfg::set_auth_enabled(cJSON_IsTrue(enabled)))
      error = "cannot enable protection without a password";

  cJSON_Delete(root);

  if (error) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", error);
    return send_json(req, r);
  }

  /* The old credentials must stop working now, not in five minutes. */
  forget_auth_cache();

  appcfg::Auth a = appcfg::auth();
  cJSON       *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddBoolToObject(r, "enabled", a.enabled);
  cJSON_AddBoolToObject(r, "has_password", a.has_password());
  return send_json(req, r);
}

/* The name doubles as the DHCP hostname, the mDNS name and the access-point
   SSID, so it has to be a valid DNS label -- not merely non-empty. Rejecting
   here beats letting the router or a phone quietly mangle it later. */
bool valid_device_name(const std::string &n) {
  if (n.empty() || n.size() > 32)
    return false;
  if (n.front() == '-' || n.back() == '-')
    return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  return true;
}

esp_err_t post_device(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *name = root ? cJSON_GetObjectItem(root, "name") : nullptr;
  if (!cJSON_IsString(name)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"name required\"}");
    return ESP_OK;
  }

  /* Lowercase rather than reject: hostnames are case-insensitive anyway, and
     someone typing a capital should not be met with an error. */
  std::string n = name->valuestring;
  for (char &c : n)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  cJSON_Delete(root);

  /* An empty name reverts to the MAC-derived default. Without this there is no
     way back to it short of a factory reset. */
  if (n.empty()) {
    appcfg::set_device_name("");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "name", appcfg::default_device_name().c_str());
    cJSON_AddBoolToObject(r, "restart_required", true);
    return send_json(req, r);
  }

  if (!valid_device_name(n)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"1-32 characters, a-z 0-9 and dashes, not starting or ending with a dash\"}");
    return ESP_OK;
  }

  if (!appcfg::set_device_name(n)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddStringToObject(r, "name", n.c_str());
  /* Hostname and mDNS are bound at interface setup, so the new name only takes
     effect on the next boot. Say so rather than let the user wonder. */
  cJSON_AddBoolToObject(r, "restart_required", true);
  return send_json(req, r);
}

/* A host name or an address: letters, digits, dots, dashes, colons (IPv6). */
bool valid_host(const std::string &h) {
  if (h.size() > 63)
    return false;
  for (char c : h)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != ':')
      return false;
  return true;
}

/* What the C library reads from TZ, and an IANA name: printable, no spaces. */
bool valid_tz(const std::string &v) {
  if (v.empty() || v.size() > 47)
    return false;
  for (char c : v)
    if (c <= ' ' || c > '~')
      return false;
  return true;
}

/* Transmit power, time server and timezone. All take effect immediately,
   without a restart -- at the meter you want to step through the power levels
   while watching the signal. */
esp_err_t post_system(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  cJSON      *root = read_body(req, body) ? cJSON_Parse(body.c_str()) : nullptr;
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *tx = cJSON_GetObjectItem(root, "tx_power_dbm");
  if (cJSON_IsNumber(tx)) {
    const int d = tx->valueint;
    if (d != 0 && (d < appcfg::kTxPowerMinDbm || d > appcfg::kTxPowerMaxDbm)) {
      cJSON_Delete(root);
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_sendstr(req, "{\"error\":\"tx_power_dbm: 0 (maximum) or 2-20\"}");
      return ESP_OK;
    }
    appcfg::set_tx_power_dbm(d);
    appnet::apply_tx_power(d);
  }

  cJSON *ntp = cJSON_GetObjectItem(root, "ntp_server");
  cJSON *tzn = cJSON_GetObjectItem(root, "tz_name");
  cJSON *tzp = cJSON_GetObjectItem(root, "tz_posix");
  if (cJSON_IsString(ntp) || cJSON_IsString(tzn) || cJSON_IsString(tzp)) {
    appcfg::Clock c = appcfg::clock();
    if (cJSON_IsString(ntp))
      c.server = ntp->valuestring;
    if (cJSON_IsString(tzn) != cJSON_IsString(tzp)) {
      cJSON_Delete(root);
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_sendstr(req, "{\"error\":\"tz_name and tz_posix go together\"}");
      return ESP_OK;
    }
    if (cJSON_IsString(tzn)) {
      c.tz_name  = tzn->valuestring;
      c.tz_posix = tzp->valuestring;
    }
    if (!valid_host(c.server) || !valid_tz(c.tz_name) || !valid_tz(c.tz_posix)) {
      cJSON_Delete(root);
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_sendstr(req, "{\"error\":\"invalid time server or timezone\"}");
      return ESP_OK;
    }
    appcfg::set_clock(c);
    appnet::apply_clock();
  }
  cJSON_Delete(root);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddNumberToObject(r, "tx_power_dbm", appcfg::tx_power_dbm());
  return send_json(req, r);
}

esp_err_t post_reset(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Erases the device's settings -- WiFi, password, name, MQTT, radio and time
     -- and reboots into the setup network. The meter reading, the tuning and
     the edge log stay: losing those costs more than entering WiFi again.
     Unlike the BOOT-button hold, which only clears what can lock you out,
     this is a deliberate, confirmable action from a browser. */
  const bool ok = appcfg::factory_reset();
  cJSON     *r  = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", "could not erase all settings");
  send_json(req, r);

  if (ok) {
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(500));
          esp_restart();
        },
        "reset", 2048, nullptr, 4, nullptr);
  }
  return ESP_OK;
}

/* The meter layer already renders this as JSON; passing the text through
   saves rebuilding the same structures in two components. */
esp_err_t send_raw_json(httpd_req_t *req, const std::string &body) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body.c_str());
  return ESP_OK;
}

esp_err_t get_meter(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_meter.state_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_meter.state_json());
}

/* {"edges":[[t_ms,synced,boot],...],"total":n}, oldest first. Streamed in
   chunks: a full log is some 32,000 entries, far more than one buffer. */
esp_err_t get_edges(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_meter.edges)
    return send_raw_json(req, "{\"edges\":[],\"total\":0}");

  const std::string l     = query_param(req, "limit");
  const size_t      limit = l.empty() ? 2000 : static_cast<size_t>(std::strtoul(l.c_str(), nullptr, 10));

  httpd_resp_set_type(req, "application/json");
  std::string buf   = "{\"edges\":[";
  bool        first = true, alive = true;
  const auto  flush = [&]() {
    if (alive && httpd_resp_send_chunk(req, buf.data(), static_cast<ssize_t>(buf.size())) != ESP_OK)
      alive = false;
    buf.clear();
    return alive;
  };
  const size_t total = g_meter.edges(limit, [&](const std::string &e) {
    if (!first)
      buf += ',';
    first = false;
    buf += e;
    return buf.size() < 1024 || flush();
  });
  char         tail[48];
  std::snprintf(tail, sizeof(tail), "],\"total\":%u}", static_cast<unsigned>(total));
  buf += tail;
  flush();
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t delete_edges(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const bool ok = g_meter.clear_edges && g_meter.clear_edges();
  if (!ok)
    httpd_resp_set_status(req, "500 Internal Server Error");
  return send_raw_json(req, ok ? "{\"ok\":true}" : "{\"error\":\"clear failed\"}");
}

esp_err_t get_tuning(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_meter.tuning_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_meter.tuning_json());
}

/* Thresholds and litre factor. This replaces reflashing on every change: in
   step 0 you vary distance against emitter resistor, and every variant wants
   new thresholds. */
esp_err_t post_tuning(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  std::string error = "unavailable";
  if (g_meter.set_tuning && g_meter.set_tuning(body, &error))
    return send_raw_json(req, "{\"ok\":true}");

  httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", error.c_str());
  return send_json(req, r);
}

/* Align the reading with the mechanical register.

   Deliberately absolute rather than a correction: what you read off is a
   reading, not a difference, and sending a correction twice -- because the
   first answer never arrived -- would double the error instead of fixing it. */
esp_err_t post_total(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON       *root = cJSON_Parse(body.c_str());
  cJSON       *m3   = root ? cJSON_GetObjectItem(root, "m3") : nullptr;
  const bool   have = cJSON_IsNumber(m3);
  const double want = have ? m3->valuedouble : 0.0;
  cJSON_Delete(root);

  std::string error = "expected {\"m3\": <number>}";
  if (have && g_meter.set_total && g_meter.set_total(want, &error))
    return send_raw_json(req, "{\"ok\":true}");

  httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", error.c_str());
  return send_json(req, r);
}

/* Hub-Fenster neu beginnen. */
esp_err_t post_reset_minmax(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (g_meter.reset_minmax)
    g_meter.reset_minmax();
  return send_raw_json(req, "{\"ok\":true}");
}

/* {"on":true} stops counting, {"on":false} resumes after a resync. */
esp_err_t post_pause(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  std::string body;
  cJSON      *root = read_body(req, body) ? cJSON_Parse(body.c_str()) : nullptr;
  cJSON      *on   = root ? cJSON_GetObjectItem(root, "on") : nullptr;
  const bool  ok   = cJSON_IsBool(on) && g_meter.set_paused;
  if (ok)
    g_meter.set_paused(cJSON_IsTrue(on));
  cJSON_Delete(root);
  if (!ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"expected {\\\"on\\\":true|false}\"}");
  }
  return send_raw_json(req, "{\"ok\":true}");
}

/* Live stream on or off. The interface calls this when opening and closing
   the scope view. */
esp_err_t post_scope(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  read_body(req, body);
  cJSON     *root = cJSON_Parse(body.c_str());
  cJSON     *on   = root ? cJSON_GetObjectItem(root, "on") : nullptr;
  const bool want = cJSON_IsBool(on) ? cJSON_IsTrue(on) : true;
  cJSON_Delete(root);

  /* The ring comes into existence here if nobody wanted it before. */
  if (want)
    ring_reserve();
  if (g_meter.set_scope)
    g_meter.set_scope(want);
  return send_raw_json(req, want ? "{\"ok\":true,\"scope\":true}" : "{\"ok\":true,\"scope\":false}");
}

/* The same ring the WebSocket replays from, but pull rather than push, so a
   script (or a second scope) can fetch recent samples over plain HTTP.
   GET /api/v1/samples?since=N returns every sample newer than N, oldest first;
   omit `since` (or pass -1) for the whole ring. "latest" is the newest sequence
   held, to pass back as `since` next time; "gap" is true when the ring no longer
   reaches all the way back to since+1. Each item is the exact JSON the monitor
   receives live, sequence number and all. */
esp_err_t get_samples(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Asking for the recent traffic is asking for the ring, so this is where it
     comes into existence if nobody has wanted it before. */
  ring_reserve();

  const std::string s     = query_param(req, "since");
  const long long   since = s.empty() ? -1 : std::strtoll(s.c_str(), nullptr, 10);

  std::vector<std::string> out;
  bool                     have = false, gap = false;
  uint32_t                 oldest = 0, newest = 0;
  if (g_ring_mux) {
    xSemaphoreTake(g_ring_mux, portMAX_DELAY);
    size_t idx = (g_ring_head + kRingCap - g_ring_count) % kRingCap;
    for (size_t i = 0; i < g_ring_count; i++) {
      const RingItem &it = g_ring[idx];
      if (i == 0)
        oldest = it.seq;
      newest = it.seq;
      have   = true;
      if (static_cast<long long>(it.seq) > since)
        out.emplace_back(it.text);
      idx = (idx + 1) % kRingCap;
    }
    if (g_ring_count > 0 && since >= 0 && static_cast<long long>(oldest) > since + 1)
      gap = true;
    xSemaphoreGive(g_ring_mux);
  }

  char head[96];
  std::snprintf(head, sizeof(head), "{\"latest\":%lld,\"oldest\":%lld,\"gap\":%s,\"items\":[",
                have ? static_cast<long long>(newest) : -1, have ? static_cast<long long>(oldest) : -1,
                gap ? "true" : "false");
  std::string body = head;
  for (size_t i = 0; i < out.size(); i++) {
    if (i)
      body += ',';
    body += out[i];
  }
  body += "]}";
  return send_raw_json(req, body);
}

esp_err_t get_health(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_diag.health_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_diag.health_json());
}

/* Streamed in chunks: a core dump is tens of kilobytes and holding it in memory
   to send it would risk the very condition it was recorded for. */
esp_err_t get_coredump(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const size_t total = g_diag.coredump_size ? g_diag.coredump_size() : 0;
  if (!total) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"error\":\"no core dump stored\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

  char   buf[1024];
  size_t sent = 0;
  while (sent < total) {
    const size_t chunk = std::min(sizeof(buf), total - sent);
    if (!g_diag.read_coredump(sent, buf, chunk)) {
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, buf, chunk) != ESP_OK)
      return ESP_FAIL;
    sent += chunk;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t delete_coredump(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const bool ok = g_diag.erase_coredump && g_diag.erase_coredump();
  cJSON     *r  = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", "nothing to erase, or erase failed");
  return send_json(req, r);
}

esp_err_t get_mqtt(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  return send_raw_json(req, appmqtt::status_json().c_str());
}

esp_err_t post_mqtt(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Roomy enough for a CA certificate: a PEM chain runs to a couple of
     kilobytes before JSON escaping, and refusing it here would look like the
     certificate was wrong rather than merely too long for the reader. */
  std::string body;
  if (!read_body(req, body, 8192)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
    return ESP_OK;
  }

  const auto str = [&](const char *k, const std::string &fallback) {
    cJSON *v = cJSON_GetObjectItem(root, k);
    return cJSON_IsString(v) ? std::string(v->valuestring) : fallback;
  };
  const auto flag = [&](const char *k, bool fallback) {
    cJSON *v = cJSON_GetObjectItem(root, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
  };

  const auto num = [&](const char *k, long fallback) {
    cJSON *v = cJSON_GetObjectItem(root, k);
    return cJSON_IsNumber(v) ? static_cast<long>(v->valuedouble) : fallback;
  };

  appcfg::Mqtt m  = appcfg::mqtt();
  m.tls           = flag("tls", m.tls);
  m.host          = str("host", m.host);
  const long port = num("port", m.port);
  m.port          = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port)
                                                : (m.tls ? appcfg::Mqtt::kPortTls : appcfg::Mqtt::kPortPlain);
  m.client_id     = str("client_id", m.client_id);
  m.auth          = flag("auth", m.auth);
  m.user          = str("user", m.user);
  /* Absent means unchanged, so the settings page can show that a password is
     stored without ever having to hold it. Empty string clears it. */
  cJSON *pw = cJSON_GetObjectItem(root, "password");
  if (cJSON_IsString(pw))
    m.password = pw->valuestring;
  m.tls_insecure     = flag("tls_insecure", m.tls_insecure);
  m.ca_cert          = str("ca_cert", m.ca_cert);
  m.base             = str("base", m.base);
  m.discovery        = flag("discovery", m.discovery);
  m.discovery_prefix = str("discovery_prefix", m.discovery_prefix);
  m.enabled          = flag("enabled", m.enabled);
  cJSON_Delete(root);

  const auto reject = [&](const char *why) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", why);
    send_json(req, r);
  };

  if (m.enabled && !m.configured()) {
    reject("a broker host is required");
    return ESP_OK;
  }
  /* Checked whether or not the client is switched on: saving credentials that
     cannot work, to be discovered later when something is turned on, is the
     kind of delay that makes a setting feel broken. */
  if (m.auth && m.user.empty()) {
    reject("a user name is required when authentication is on");
    return ESP_OK;
  }
  if (m.auth && m.password.empty()) {
    reject("a password is required when authentication is on");
    return ESP_OK;
  }
  /* No certificate with TLS: checked against the built-in public authorities. */
  /* A certificate that is not PEM would be refused deep inside the TLS stack at
     connect time, as a failed handshake with nothing to point at. */
  if (m.tls && !m.tls_insecure && !m.ca_cert.empty() &&
      m.ca_cert.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
    reject("the CA certificate must be PEM, starting with -----BEGIN CERTIFICATE-----");
    return ESP_OK;
  }

  if (!appcfg::set_mqtt(m)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");

  /* Reconnect after the response, or the client watches its own connection die
     while waiting for an answer that is already written. */
  static TaskHandle_t apply_task = nullptr;
  if (!apply_task) {
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(500));
          appmqtt::reconfigure();
          apply_task = nullptr;
          vTaskDelete(nullptr);
        },
        "mqtt_apply", 4096, nullptr, 4, &apply_task);
  }
  return ESP_OK;
}

esp_err_t post_restart(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  httpd_resp_sendstr(req, "{\"ok\":true}");
  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      },
      "restart", 2048, nullptr, 4, nullptr);
  return ESP_OK;
}

/* Captive-portal probes: anything we do not serve gets redirected to the setup
   page, which is what makes phones pop up the "sign in to network" sheet.

   Except under /api/, where a redirect to an HTML page is a confusing answer to
   a mistyped endpoint. Something expecting JSON should be told it was wrong, in
   JSON. */
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t) {
  if (std::strncmp(req->uri, "/api/", 5) == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"no such endpoint\"}");
    return ESP_OK;
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

// --------------------------------------------------------------- websocket ----

/* There is deliberately no local table of connected monitors.

   ESP-IDF 6 does not call the URI handler once a WebSocket handshake has
   succeeded -- httpd_uri.c returns early with "If the request is websocket
   handshake, then do not call the uri->handler". The widely copied idiom of
   registering the socket inside an `if (req->method == HTTP_GET)` branch (still
   what the ws_echo_server example shows) therefore never runs on IDF 6, and the
   monitor delivered nothing at all while looking perfectly connected: the
   server completes the handshake and answers 101 regardless.

   Asking the server who is attached avoids the whole class of problem. There
   are no stale descriptors when a browser tab dies, no slots to leak, and
   nothing that has to stay in step with a handler-invocation rule that has
   already changed under this code once. */
size_t ws_clients(int *out, size_t max) {
  if (!g_server)
    return 0;

  int    fds[kMaxSockets];
  size_t n = kMaxSockets;
  if (httpd_get_client_list(g_server, &n, fds) != ESP_OK)
    return 0;

  size_t found = 0;
  for (size_t i = 0; i < n && found < max; i++)
    if (httpd_ws_get_fd_info(g_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
      out[found++] = fds[i];
  return found;
}

/* Runs before the 101 is sent, which makes it the only place an upgrade can
   still be refused. Rejecting after the handshake would leave the client
   holding a socket that silently never delivers -- precisely the failure this
   section exists to prevent. Authenticating here also closes a real hole: the
   check used to sit in the dead HTTP_GET branch above, so with a password set
   the monitor was still reachable without one. */
esp_err_t ws_pre_handshake(httpd_req_t *req) {
  if (check_auth(req)) {
    /* Nagle off: the stream is many small frames, and holding each back until
       the previous one is acknowledged -- which a browser delays on purpose --
       turned 50 evenly spaced samples a second into clumps with pauses of up
       to a second. */
    int one = 1;
    setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
  }
  deny(req);
  return ESP_FAIL;
}

/* Send one text message to a single socket, synchronously. Safe from here: the
   server runs one HTTP task, so while this handler is on it no queued broadcast
   can be writing to the same socket in parallel. */
void ws_send_text(httpd_req_t *req, const char *text) {
  httpd_ws_frame_t frame = {};
  frame.final            = true;
  frame.type             = HTTPD_WS_TYPE_TEXT;
  frame.payload          = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
  frame.len              = std::strlen(text);
  httpd_ws_send_frame(req, &frame);
}

/* A monitor announces the last sequence number it holds as {"since":N} (N = -1
   means it holds nothing). Everything newer still in the ring is sent straight
   back to that one socket, oldest first, so a reconnect resumes exactly where it
   left off before the live stream carries on. If the gap reaches further back
   than the ring keeps, a {"t":"gap"} marker says so rather than letting the
   history look complete when it is not. */
void replay_since(httpd_req_t *req, const char *msg) {
  const char *p = std::strstr(msg, "\"since\"");
  if (!p)
    return;
  ring_reserve();
  p += 7;
  while (*p == ':' || *p == ' ')
    p++;
  const long long since = std::strtoll(p, nullptr, 10);

  std::vector<std::string> out;
  bool                     gap = false;
  if (g_ring_mux) {
    xSemaphoreTake(g_ring_mux, portMAX_DELAY);
    size_t   idx    = (g_ring_head + kRingCap - g_ring_count) % kRingCap;
    uint32_t oldest = 0;
    for (size_t i = 0; i < g_ring_count; i++) {
      const RingItem &it = g_ring[idx];
      if (i == 0)
        oldest = it.seq;
      if (static_cast<long long>(it.seq) > since)
        out.emplace_back(it.text);
      idx = (idx + 1) % kRingCap;
    }
    if (g_ring_count > 0 && since >= 0 && static_cast<long long>(oldest) > since + 1)
      gap = true;
    xSemaphoreGive(g_ring_mux);
  }

  if (gap)
    ws_send_text(req, "{\"t\":\"gap\"}");
  for (const std::string &s : out)
    ws_send_text(req, s.c_str());
}

esp_err_t ws_handler(httpd_req_t *req) {

  /* Clients say one thing only: {"since":N}, sent on connect to be handed what
     they missed from the ring (see replay_since). Anything else is ignored, but
     the frame still has to be read -- leaving the payload in the socket buffer
     makes the server call straight back in and spin at full speed, and a browser
     closing its tab was once enough to hang the device. */
  httpd_ws_frame_t frame = {};
  esp_err_t        err   = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK)
    return err;

  /* Whatever arrives has to leave the socket buffer, including what we have no
     use for. Reading only the frames small enough to be a message left anything
     larger sitting there, and the server would call straight back in and spin
     -- exactly the hang described above, reachable by sending 512 bytes.

     Beyond a few kilobytes the connection is dropped rather than drained: no
     legitimate message is that size, and allocating whatever a caller claims
     to have sent is the other way to be knocked over. */
  constexpr size_t kWsMessageMax = 512;  // longest thing a client legitimately says
  constexpr size_t kWsDrainMax   = 4096; // beyond this, close instead of read

  if (frame.len > kWsDrainMax) {
    ESP_LOGW(TAG, "WebSocket frame of %u bytes; closing the connection", static_cast<unsigned>(frame.len));
    return ESP_FAIL;
  }

  if (frame.len > 0) {
    std::vector<uint8_t> buf(frame.len + 1);
    frame.payload = buf.data();
    err           = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK)
      return err;
    buf[frame.len] = 0;
    if (frame.len < kWsMessageMax)
      replay_since(req, reinterpret_cast<const char *>(buf.data()));
  }

  /* A CLOSE needs no bookkeeping: the server drops the socket, and the next
     broadcast simply does not find it any more. */
  return ESP_OK;
}

/* One outgoing message, shared by every client it goes to.

   The async send copies only the frame header, not the payload: the payload
   is read later, on the HTTP task. Pointing it at the pump's own buffer -- as
   this did -- let the next message overwrite it before it had gone out, and
   the browser got torn JSON it silently threw away. So each broadcast gets
   its own copy, freed once the last client has been served. */
struct Out {
  std::atomic<int> refs;
  size_t           len;
  char             data[1];
};
std::atomic<int> g_in_flight{0};
constexpr int    kMaxInFlight = 8; // beyond this a client is not keeping up: drop, do not queue

void out_release(Out *o) {
  if (o->refs.fetch_sub(1) == 1) {
    free(o);
    g_in_flight--;
  }
}

/* A send that failed means the client is gone or stuck: close it, or every
   later send would wait out the timeout on the one HTTP task, and nothing
   else -- status, other browsers, a firmware upload -- would get through. */
void on_sent(esp_err_t err, int sock, void *arg) {
  if (err != ESP_OK)
    httpd_sess_trigger_close(g_server, sock);
  out_release(static_cast<Out *>(arg));
}

void broadcast(const char *text) {
  int          fds[kMaxSockets];
  const size_t n = ws_clients(fds, kMaxSockets);
  if (n == 0)
    return;
  if (g_in_flight.load() >= kMaxInFlight) {
    g_dropped++;
    return;
  }

  const size_t len = std::strlen(text);
  Out         *o   = static_cast<Out *>(malloc(sizeof(Out) + len));
  if (!o) {
    g_dropped++;
    return;
  }
  new (&o->refs) std::atomic<int>(static_cast<int>(n));
  o->len = len;
  std::memcpy(o->data, text, len + 1);
  g_in_flight++;

  httpd_ws_frame_t frame = {};
  frame.final            = true;
  frame.type             = HTTPD_WS_TYPE_TEXT;
  frame.payload          = reinterpret_cast<uint8_t *>(o->data);
  frame.len              = len;

  for (size_t i = 0; i < n; i++)
    if (httpd_ws_send_data_async(g_server, fds[i], &frame, on_sent, o) != ESP_OK)
      out_release(o); // never queued: its share is ours to give back
}

bool any_client() {
  int fds[kMaxSockets];
  return ws_clients(fds, kMaxSockets) > 0;
}

/* Owns all sockets. Producers only ever push into the queue, so the sampler
   task can never block on a slow or vanished HTTP client. */
/* Samples go out bundled: whatever has queued up by the time the first one is
   taken, up to kBundle, as one JSON array. A tenth of the frames for the same
   data -- less airtime, and a short hold-up on the air no longer turns into a
   visible clump. Single messages still go out as they are. */
constexpr size_t     kBundle     = 10;
constexpr TickType_t kBundleWait = pdMS_TO_TICKS(100);

void ws_pump(void *) {
  Msg         msg;
  static char bundle[kBundle * kMsgText + 8];
  for (;;) {
    if (xQueueReceive(g_queue, &msg, pdMS_TO_TICKS(200)) == pdPASS) {
      /* Give the rest of a bundle up to kBundleWait to arrive. */
      size_t n = 1, len = 0;
      bundle[len++] = '[';
      len += snprintf(bundle + len, sizeof(bundle) - len, "%s", msg.text);
      const TickType_t start = xTaskGetTickCount();
      while (n < kBundle) {
        const TickType_t elapsed = xTaskGetTickCount() - start; // wraps safely
        if (elapsed >= kBundleWait || xQueueReceive(g_queue, &msg, kBundleWait - elapsed) != pdPASS)
          break;
        len += snprintf(bundle + len, sizeof(bundle) - len, ",%s", msg.text);
        n++;
      }
      bundle[len++] = ']';
      bundle[len]   = '\0';
      if (any_client())
        broadcast(bundle);
    }
    if (g_dropped.load() && any_client()) {
      char note[64];
      snprintf(note, sizeof(note), "{\"t\":\"drop\",\"n\":%" PRIu32 "}", g_dropped.exchange(0));
      broadcast(note);
    }
  }
}

} // namespace

void publish_sample(int mv, bool edge, int64_t t_us) {
  if (!g_queue)
    return;

  Msg msg;
  /* g_seq is only touched here, by the sampler task, so the counter needs no
     lock; the ring it feeds has its own. */
  const uint32_t seq = g_seq++;
  snprintf(msg.text, sizeof(msg.text), "{\"t\":\"s\",\"s\":%u,\"ts\":%lld,\"v\":%d,\"e\":%d}",
           static_cast<unsigned>(seq), static_cast<long long>(t_us / 1000), mv, edge ? 1 : 0);
  ring_append(seq, msg.text);

  /* Never block: a scope that cannot keep up loses samples, the sampler loses
     no time. It catches up from the ring on reconnect. */
  if (xQueueSend(g_queue, &msg, 0) != pdPASS)
    g_dropped++;
}

bool begin(StatusProvider provider, OtaHook ota_hook, MeterApi meter, DiagnosticsApi diagnostics) {
  g_status   = std::move(provider);
  g_ota_hook = std::move(ota_hook);
  g_meter    = std::move(meter);
  g_diag     = std::move(diagnostics);

  g_queue = xQueueCreate(kQueueLen, sizeof(Msg));
  if (!g_queue) {
    ESP_LOGE(TAG, "queue alloc failed");
    return false;
  }

  g_ring_mux = xSemaphoreCreateMutex();
  if (!g_ring_mux) {
    ESP_LOGE(TAG, "ring mutex alloc failed");
    return false;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  static_assert(kMaxSockets == 7, "ws_clients() reads back this many sockets");
  cfg.max_open_sockets = kMaxSockets;
  cfg.max_uri_handlers = 32; /* 29 in use */
  cfg.lru_purge_enable = true;
  cfg.stack_size       = 6144;
  /* No close_fn: with the client list queried from the server there is nothing
     left to clean up, and setting it would make closing the socket our job. */
  /* Firmware uploads arrive in one long request; the default receive timeout is
     too short for a slow uplink. */
  cfg.recv_wait_timeout = 20;
  /* Sending, by contrast, must not wait long: all sends run on the one HTTP
     task, and at 20 s a stuck WebSocket client held up every other request --
     a firmware upload timed out behind it. */
  cfg.send_wait_timeout = 3;
  /* And peers that vanished without closing (tab slept, WiFi gone) are found
     by TCP keepalive after about 11 s instead of never. */
  cfg.keep_alive_enable   = true;
  cfg.keep_alive_idle     = 5;
  cfg.keep_alive_interval = 2;
  cfg.keep_alive_count    = 3;

  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return false;
  }

  /* Designated initialisers on purpose: httpd_uri_t has grown fields across
     IDF versions, and positional ones break silently when it does. */
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = get_index},
      {.uri = "/api/v1/info", .method = HTTP_GET, .handler = get_info},
      {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = get_wifi_scan},
      {.uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = post_wifi_connect},
      {.uri = "/api/v1/config/auth", .method = HTTP_POST, .handler = post_auth},
      {.uri = "/api/v1/config/device", .method = HTTP_POST, .handler = post_device},
      {.uri = "/api/v1/config/mqtt", .method = HTTP_GET, .handler = get_mqtt},
      {.uri = "/api/v1/config/mqtt", .method = HTTP_POST, .handler = post_mqtt},
      {.uri = "/api/v1/config/system", .method = HTTP_POST, .handler = post_system},
      {.uri = "/api/v1/restart", .method = HTTP_POST, .handler = post_restart},
      {.uri = "/api/v1/reset", .method = HTTP_POST, .handler = post_reset},
      {.uri = "/api/v1/ota", .method = HTTP_POST, .handler = post_ota},
      {.uri = "/api/v1/ota/status", .method = HTTP_GET, .handler = get_ota_status},
      {.uri = "/api/v1/ota/confirm", .method = HTTP_POST, .handler = post_ota_confirm},
      {.uri = "/api/v1/selftest", .method = HTTP_GET, .handler = get_selftest},
      {.uri = "/api/v1/meter", .method = HTTP_GET, .handler = get_meter},
      {.uri = "/api/v1/meter/total", .method = HTTP_POST, .handler = post_total},
      {.uri = "/api/v1/edges", .method = HTTP_GET, .handler = get_edges},
      {.uri = "/api/v1/edges", .method = HTTP_DELETE, .handler = delete_edges},
      {.uri = "/api/v1/tuning", .method = HTTP_GET, .handler = get_tuning},
      {.uri = "/api/v1/tuning", .method = HTTP_POST, .handler = post_tuning},
      {.uri = "/api/v1/scope", .method = HTTP_POST, .handler = post_scope},
      {.uri = "/api/v1/minmax/reset", .method = HTTP_POST, .handler = post_reset_minmax},
      {.uri = "/api/v1/meter/pause", .method = HTTP_POST, .handler = post_pause},
      {.uri = "/api/v1/samples", .method = HTTP_GET, .handler = get_samples},
      {.uri = "/api/v1/health", .method = HTTP_GET, .handler = get_health},
      {.uri = "/api/v1/coredump", .method = HTTP_GET, .handler = get_coredump},
      {.uri = "/api/v1/coredump", .method = HTTP_DELETE, .handler = delete_coredump},
      {.uri                 = "/api/v1/ws",
       .method              = HTTP_GET,
       .handler             = ws_handler,
       .is_websocket        = true,
       .ws_pre_handshake_cb = ws_pre_handshake},
  };
  for (const auto &r : routes)
    httpd_register_uri_handler(g_server, &r);

  httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, not_found);

  if (xTaskCreate(ws_pump, "ws_pump", 4096, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "ws_pump task failed");
    return false;
  }

  ESP_LOGI(TAG, "HTTP server up, auth %s", auth_required() ? "on" : "off");
  return true;
}

bool ui_confirmed() {
  return g_ui_confirmed.load();
}

void set_probation(const Probation &p) {
  std::lock_guard<std::mutex> l(g_probation_mx);
  g_probation = p;
}

bool self_check() {
  /* Over loopback, through the real server and its real routing: a server
     that is up but cannot answer -- a handler table that overflowed, a task
     that hangs -- fails here, where checking a flag would not. */
  const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0)
    return false;
  timeval tv{2, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in a{};
  a.sin_family      = AF_INET;
  a.sin_port        = htons(80);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok           = false;
  if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0) {
    static const char kReq[] = "GET /api/v1/selftest HTTP/1.0\r\n\r\n";
    if (send(s, kReq, sizeof(kReq) - 1, 0) == static_cast<int>(sizeof(kReq) - 1)) {
      /* Headers and body may arrive as separate segments: read until the
         body is there, the server closes, or the timeout hits. */
      char buf[384] = {};
      int  got      = 0;
      while (got < static_cast<int>(sizeof(buf)) - 1) {
        const int n = recv(s, buf + got, sizeof(buf) - 1 - got, 0);
        if (n <= 0)
          break;
        got += n;
        if (std::strstr(buf, "\"ok\":true"))
          break;
      }
      ok = got > 12 && std::strncmp(buf + 9, "200", 3) == 0 && std::strstr(buf, "\"ok\":true");
    }
  }
  close(s);
  return ok;
}

} // namespace appweb
