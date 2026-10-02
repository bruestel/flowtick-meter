/*
   WiFi with an access-point fallback, plus a captive-portal DNS responder.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "appnet/Net.h"
#include "appcfg/Config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mdns.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/time.h>

namespace appnet {
namespace {

const char *const TAG = "appnet";

/* Six attempts five seconds apart, so roughly half a minute of patience before
   the setup network appears. Retrying without a pause -- as this did at first --
   burns all attempts in a couple of seconds and rides out nothing at all. */
constexpr int      kMaxStaRetries = 6;
constexpr int64_t  kStaRetryUs    = 5 * 1000000LL;
constexpr uint16_t kDnsPort       = 53;
/* The access point's fixed address: what the DNS server binds to, what it
   answers with, and what it reports as the device's address while in setup
   mode. One constant so the three cannot disagree. */
constexpr const char *kApAddress = "192.168.4.1";

/* After a successful join the setup network lingers briefly. Whoever just
   entered the credentials is still connected to it and needs to see that it
   worked -- and to read the new address -- before it disappears under them. */
constexpr int64_t kApLingerUs = 120 * 1000000LL;

/* While the setup network is up the station side still tries to get back on the
   real one, just slowly. Without this the device would sit in access-point mode
   forever after a router reboot, and someone would have to go and fetch it. */
constexpr int64_t kApRetryUs = 30 * 1000000LL;

State              g_state = State::Idle;
StateCallback      g_cb;
int                g_retries         = 0;
esp_netif_t       *g_sta_netif       = nullptr;
esp_netif_t       *g_ap_netif        = nullptr;
char               g_ip[16]          = "";
TaskHandle_t       g_dns_task        = nullptr;
volatile bool      g_dns_stop        = false;
bool               g_ap_active       = false;
esp_timer_handle_t g_ap_linger_timer = nullptr;
esp_timer_handle_t g_ap_retry_timer  = nullptr;
esp_timer_handle_t g_sta_retry_timer = nullptr;

bool g_time_synced = false;

/* Trying credentials before storing them, see test_credentials(). */
constexpr int64_t kTestTimeoutUs = 20 * 1000000LL;
/* After a successful test the page already has the new address and moves on
   by itself, so the setup network needs to outlive it only briefly. */
constexpr int64_t kTestLingerUs = 30 * 1000000LL;
/* Station to station, the new address is found out and then carried back to
   the old network, where the page that asked is still waiting -- two
   networks may hand out addresses from different subnets, and once the device
   has left, nobody on the old one could tell the page where it went. This is
   how long it stays back there before switching for good. */
constexpr int64_t  kCommitUs = 12 * 1000000LL;
std::mutex         g_test_mx;
Test               g_test;            // what the interface reads, under g_test_mx
bool               g_testing = false; // event handler's view; set and cleared with g_test
std::string        g_test_pass;
esp_timer_handle_t g_test_timer           = nullptr;
esp_timer_handle_t g_test_start_timer     = nullptr;
esp_timer_handle_t g_commit_timer         = nullptr;
bool               g_restart_after_linger = false;
int64_t            g_linger_until_us      = 0;

void on_time_sync(struct timeval *tv) {
  char         buf[32] = "";
  const time_t secs    = tv ? tv->tv_sec : 0;
  struct tm    utc;
  gmtime_r(&secs, &utc);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc);
  ESP_LOGI(TAG, "Clock %s from NTP: %s UTC", g_time_synced ? "corrected" : "set", buf);
  g_time_synced = true;
}

/* Started once the station has an address, never in AP mode -- there is no
   route to a time server on our own network, and the attempts would just be
   noise in the log.

   Two servers: the one from the settings, and a built-in one behind it that
   is always there, so a mistyped or vanished server costs nothing but the
   first few seconds. With the setting left empty both slots are built in. */
constexpr const char *kFallbackA = "pool.ntp.org";
constexpr const char *kFallbackB = "time.cloudflare.com";
/* SNTP keeps the pointers, so the names have to outlive the call. */
std::string g_ntp[2];
bool        g_sntp_started = false;
bool        g_sntp_wanted  = false;
/* The event loop starts SNTP, the web server restarts it on a new setting. */
std::mutex g_sntp_mx;

void start_sntp_locked() {
  if (g_sntp_started)
    return;

  const std::string own = appcfg::clock().server;
  g_ntp[0]              = own.empty() ? kFallbackA : own;
  g_ntp[1]              = g_ntp[0] == kFallbackA ? kFallbackB : kFallbackA;

  esp_sntp_config_t cfg =
      ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST(g_ntp[0].c_str(), g_ntp[1].c_str()));
  cfg.sync_cb = on_time_sync;
  /* Nothing here may block: this runs on the event loop, and the sampler must
     keep running whether or not any time server ever answers. */
  cfg.wait_for_sync = false;

  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SNTP init failed: %s -- timestamps stay relative to boot", esp_err_to_name(err));
    return;
  }
  g_sntp_started = true;
  ESP_LOGI(TAG, "SNTP started: %s, fallback %s", g_ntp[0].c_str(), g_ntp[1].c_str());
}

void start_sntp() {
  std::lock_guard<std::mutex> lock(g_sntp_mx);
  g_sntp_wanted = true;
  start_sntp_locked();
}

void set_state(State s) {
  if (s == g_state)
    return;
  g_state = s;
  ESP_LOGI(TAG, "state: %s", to_string(s));
  if (g_cb)
    g_cb(s);
}

void dns_task(void *);

/* Only while the setup network is up. The task exits within half a second of
   the flag being set, which is its receive timeout. */
void stop_dns() {
  if (!g_dns_task)
    return;
  g_dns_stop = true;
  g_dns_task = nullptr;
}

void start_dns() {
  if (g_dns_task)
    return;
  g_dns_stop = false;
  if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 3, &g_dns_task) != pdPASS) {
    g_dns_task = nullptr;
    ESP_LOGW(TAG, "Captive portal DNS could not start");
  }
}

void stop_ap() {
  if (!g_ap_active)
    return;
  g_ap_active = false;
  stop_dns();
  if (g_ap_retry_timer)
    esp_timer_stop(g_ap_retry_timer);
  ESP_LOGI(TAG, "Setup network closed; reachable at %s", g_ip);
  esp_wifi_set_mode(WIFI_MODE_STA);
}

void start_ap() {
  appcfg::Device dev = appcfg::device();

  if (g_ap_linger_timer)
    esp_timer_stop(g_ap_linger_timer);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  g_ap_active = true;
  start_dns();

  wifi_config_t     ap   = {};
  const std::string ssid = dev.name;
  std::memcpy(ap.ap.ssid, ssid.data(), std::min(ssid.size(), sizeof(ap.ap.ssid)));
  ap.ap.ssid_len       = static_cast<uint8_t>(std::min(ssid.size(), sizeof(ap.ap.ssid)));
  ap.ap.channel        = 1;
  ap.ap.max_connection = 4;
  /* Open on purpose: this network exists only so the user can get back in, and
     a password they cannot look up anywhere would defeat that. It grants access
     to this device's setup and nothing beyond it. */
  ap.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_LOGI(TAG, "Access point \"%s\" open at %s", ssid.c_str(), kApAddress);
  std::strncpy(g_ip, kApAddress, sizeof(g_ip) - 1);
  set_state(State::ApMode);

  /* Only worth retrying if there is something to retry with. */
  if (appcfg::wifi().configured() && g_ap_retry_timer) {
    esp_timer_stop(g_ap_retry_timer);
    esp_timer_start_periodic(g_ap_retry_timer, kApRetryUs);
    ESP_LOGI(TAG, "Will keep trying \"%s\" every %llds", appcfg::wifi().ssid.c_str(), kApRetryUs / 1000000);
  }
}

void try_connect() {
  appcfg::Wifi w = appcfg::wifi();
  if (!w.configured()) {
    ESP_LOGI(TAG, "No WiFi credentials stored");
    start_ap();
    return;
  }

  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, w.ssid.data(), std::min(w.ssid.size(), sizeof(sta.sta.ssid)));
  std::memcpy(sta.sta.password, w.password.data(), std::min(w.password.size(), sizeof(sta.sta.password)));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  g_retries = 0;
  set_state(State::Connecting);
  esp_wifi_connect();
}

/* Back to what was stored before the test, so the setup network's background
   retry keeps trying the old network rather than the rejected one. */
void end_test_failed(const char *why) {
  {
    std::lock_guard<std::mutex> l(g_test_mx);
    if (!g_testing)
      return;
    g_testing    = false;
    g_test.state = TestState::Failed;
    g_test.error = why;
  }
  if (g_test_timer)
    esp_timer_stop(g_test_timer);
  g_test_pass.clear();
  ESP_LOGW(TAG, "Test join failed: %s", why);
  esp_wifi_disconnect();

  const appcfg::Wifi w   = appcfg::wifi();
  wifi_config_t      sta = {};
  std::memcpy(sta.sta.ssid, w.ssid.data(), std::min(w.ssid.size(), sizeof(sta.sta.ssid)));
  std::memcpy(sta.sta.password, w.password.data(), std::min(w.password.size(), sizeof(sta.sta.password)));
  esp_wifi_set_config(WIFI_IF_STA, &sta);

  if (g_ap_active) {
    if (w.configured() && g_ap_retry_timer)
      esp_timer_start_periodic(g_ap_retry_timer, kApRetryUs);
  } else {
    /* Tried from a working network: go back to it, so the page that asked is
       able to read why it did not work. */
    ESP_LOGI(TAG, "Returning to \"%s\"", w.ssid.c_str());
    g_retries = 0;
    set_state(State::Connecting);
    esp_wifi_connect();
  }
}

/* The reasons that mean "the password is wrong" rather than "try again". A
   WPA2 router that does not like the key usually just lets the handshake time
   out, which is why the timeouts are in here. */
bool is_auth_failure(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
      return true;
    default:
      return false;
  }
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
    if (g_testing) {
      const uint8_t reason = ev ? ev->reason : 0;
      if (is_auth_failure(reason))
        end_test_failed("wrong password");
      else if (reason == WIFI_REASON_NO_AP_FOUND)
        end_test_failed("network not found");
      else
        esp_wifi_connect(); // anything else: keep trying until the timeout
      return;
    }
    /* In access-point mode the retry timer drives reconnection attempts, so
       reacting to every failure here would only produce a tight loop. */
    if (g_state == State::ApMode)
      return;
    if (++g_retries <= kMaxStaRetries) {
      ESP_LOGW(TAG, "Disconnected (reason %d), retry %d/%d in %llds", ev ? ev->reason : 0, g_retries, kMaxStaRetries,
               kStaRetryUs / 1000000);
      set_state(State::Connecting);
      if (g_sta_retry_timer) {
        esp_timer_stop(g_sta_retry_timer);
        esp_timer_start_once(g_sta_retry_timer, kStaRetryUs);
      } else {
        esp_wifi_connect();
      }
    } else {
      ESP_LOGW(TAG, "Could not join \"%s\", opening setup network", appcfg::wifi().ssid.c_str());
      start_ap();
    }
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    std::snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&event->ip_info.ip));
    const char *announced = nullptr;
    esp_netif_get_hostname(g_sta_netif, &announced);
    ESP_LOGI(TAG, "Connected, IP %s, hostname \"%s\"", g_ip, announced ? announced : "(unset)");

    /* Whether HE actually got negotiated depends on the access point too, so
       report what came out rather than what we asked for. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      ESP_LOGI(TAG, "Link: %s, rssi %d, channel %d",
               ap.phy_11ax  ? "WiFi 6 (11ax)"
               : ap.phy_11n ? "WiFi 4 (11n)"
                            : "legacy",
               ap.rssi, ap.primary);
      if (!ap.phy_11ax)
        ESP_LOGI(TAG, "Access point did not offer 802.11ax on 2.4 GHz");
    }
    bool tested = false;
    {
      std::lock_guard<std::mutex> l(g_test_mx);
      if (g_testing) {
        g_testing           = false;
        tested              = true;
        g_test.state        = TestState::Ok;
        g_test.ip           = g_ip;
        g_test.restart_in_s = (g_ap_active ? kTestLingerUs : kCommitUs) / 1000000;
      }
    }
    if (tested && !g_ap_active) {
      if (g_test_timer)
        esp_timer_stop(g_test_timer);
      /* Probe done: back to the old network to report the address, and only
         then over for good -- see kCommitUs. Nothing stored yet. */
      const appcfg::Wifi w = appcfg::wifi();
      ESP_LOGI(TAG, "\"%s\" works (%s); back to \"%s\" to report it, switching in %llds", g_test.ssid.c_str(), g_ip,
               w.ssid.c_str(), kCommitUs / 1000000);
      g_linger_until_us = esp_timer_get_time() + kCommitUs;
      g_retries         = 0;
      esp_wifi_disconnect();
      wifi_config_t sta = {};
      std::memcpy(sta.sta.ssid, w.ssid.data(), std::min(w.ssid.size(), sizeof(sta.sta.ssid)));
      std::memcpy(sta.sta.password, w.password.data(), std::min(w.password.size(), sizeof(sta.sta.password)));
      esp_wifi_set_config(WIFI_IF_STA, &sta);
      esp_wifi_connect();
      esp_timer_start_once(g_commit_timer, kCommitUs);
      return;
    }
    if (tested) {
      if (g_test_timer)
        esp_timer_stop(g_test_timer);
      /* Only now: these credentials have produced an address. */
      appcfg::set_wifi(g_test.ssid, g_test_pass);
      g_test_pass.clear();
      /* Only off the setup network. Station to station has no access point
         to get in the way, which is what the restart is for. */
      g_restart_after_linger = g_ap_active;
      ESP_LOGI(TAG, "Test join worked, credentials for \"%s\" stored", g_test.ssid.c_str());
    }

    g_retries = 0;
    if (g_ap_retry_timer)
      esp_timer_stop(g_ap_retry_timer);
    if (g_sta_retry_timer)
      esp_timer_stop(g_sta_retry_timer);
    set_state(State::Connected);
    start_sntp();

    /* Advertise only once we are on the real network -- announcing a name on
       our own access point would just point back at ourselves. */
    static bool mdns_up = false;
    if (!mdns_up) {
      const std::string host = appcfg::device().name;
      if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host.c_str());
        mdns_instance_name_set("flowtick water meter");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        ESP_LOGI(TAG, "Also reachable at http://%s.local/", host.c_str());
        mdns_up = true;
      } else {
        ESP_LOGW(TAG, "mDNS unavailable; use the IP address");
      }
    }

    if (g_ap_active && g_ap_linger_timer) {
      const int64_t linger = g_restart_after_linger ? kTestLingerUs : kApLingerUs;
      ESP_LOGI(TAG, "Closing the setup network in %llds%s", linger / 1000000,
               g_restart_after_linger ? ", then restarting" : "");
      esp_timer_stop(g_ap_linger_timer);
      esp_timer_start_once(g_ap_linger_timer, linger);
      g_linger_until_us = esp_timer_get_time() + linger;
    }
  }
}

/* Answers every A query with our own address so any browser request lands on
   the setup page. Deliberately minimal: it only needs to fool a captive-portal
   probe, not to be a DNS server. */
void dns_task(void *) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket failed");
    vTaskDelete(nullptr);
    return;
  }

  /* The access point's own address, not INADDR_ANY. This server answers every
     query with 192.168.4.1, which is the point of a captive portal and a lie
     everywhere else: bound to all interfaces it would also answer on the home
     network, where anything that happened to ask it -- or a router probing for
     DNS rebinding -- would be told that every name resolves to the setup
     page. */
  sockaddr_in addr     = {};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = inet_addr(kApAddress);
  addr.sin_port        = htons(kDnsPort);
  if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed");
    close(sock);
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  /* So the loop notices it has been asked to stop; without a timeout it would
     sit in recvfrom until the next query, which on a closed access point never
     comes. */
  timeval rcv_timeout = {.tv_sec = 0, .tv_usec = 500000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

  uint8_t buf[512];
  while (!g_dns_stop) {
    sockaddr_in from     = {};
    socklen_t   from_len = sizeof(from);
    int         len      = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
    if (len < 12)
      continue;

    /* Only answer standard queries with exactly one question. */
    const uint16_t qdcount = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
    if ((buf[2] & 0x80) || qdcount != 1)
      continue;

    /* Walk the QNAME labels to find where the question ends. */
    int pos = 12;
    while (pos < len && buf[pos] != 0) {
      if ((buf[pos] & 0xC0) == 0xC0) {
        pos += 2;
        break;
      } // compression pointer
      pos += buf[pos] + 1;
    }
    if (pos >= len)
      continue;
    pos += 1 + 4; // terminating zero + QTYPE + QCLASS
    if (pos > len || pos + 16 > static_cast<int>(sizeof(buf)))
      continue;

    buf[2]  = 0x81; // response, recursion desired
    buf[3]  = 0x80; // recursion available, no error
    buf[6]  = 0;
    buf[7]  = 1; // one answer
    buf[8]  = 0;
    buf[9]  = 0; // no authority
    buf[10] = 0;
    buf[11] = 0; // no additional

    uint8_t *a        = buf + pos;
    a[0]              = 0xC0;
    a[1]              = 0x0C; // name: pointer to the question
    a[2]              = 0x00;
    a[3]              = 0x01; // type A
    a[4]              = 0x00;
    a[5]              = 0x01; // class IN
    a[6]              = 0;
    a[7]              = 0;
    a[8]              = 0;
    a[9]              = 30; // TTL 30 s
    a[10]             = 0x00;
    a[11]             = 0x04;                  // rdlength
    const uint32_t ap = inet_addr(kApAddress); // already network byte order
    std::memcpy(a + 12, &ap, 4);

    sendto(sock, buf, pos + 16, 0, reinterpret_cast<sockaddr *>(&from), from_len);
  }

  close(sock);
  vTaskDelete(nullptr);
}

} // namespace

const char *to_string(State s) {
  switch (s) {
    case State::Idle:
      return "idle";
    case State::Connecting:
      return "connecting";
    case State::Connected:
      return "connected";
    case State::ApMode:
      return "ap-mode";
  }
  return "?";
}

bool begin(StateCallback cb) {
  g_cb = std::move(cb);
  apply_clock();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_sta_netif = esp_netif_create_default_wifi_sta();
  g_ap_netif  = esp_netif_create_default_wifi_ap();

  /* Set once, right after the interface exists and long before DHCP runs, so
     the name is in the very first DHCP request rather than a later renewal. */
  const std::string host = appcfg::device().name;
  esp_err_t         hn   = esp_netif_set_hostname(g_sta_netif, host.c_str());
  if (hn != ESP_OK)
    ESP_LOGW(TAG, "Could not set hostname: %s", esp_err_to_name(hn));

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

  /* Station only to begin with. The access point is not a permanent feature --
     it is opened when there is no other way in, and closed again once there is.
     Leaving it up would put a second, open network on the air for no reason.
     While it is up the mode is APSTA, so the station side keeps retrying in the
     background and the device rejoins by itself when the router returns. */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  const esp_timer_create_args_t linger = {
      .callback =
          [](void *) {
            if (g_restart_after_linger) {
              ESP_LOGI(TAG, "Restarting onto the new network");
              esp_restart();
            }
            stop_ap();
          },
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "ap_linger",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&linger, &g_ap_linger_timer);

  const esp_timer_create_args_t retry = {
      .callback =
          [](void *) {
            if (g_ap_active) {
              ESP_LOGI(TAG, "Retrying \"%s\"", appcfg::wifi().ssid.c_str());
              esp_wifi_connect();
            }
          },
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "ap_retry",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&retry, &g_ap_retry_timer);

  const esp_timer_create_args_t sta_retry = {
      .callback              = [](void *) { esp_wifi_connect(); },
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "sta_retry",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&sta_retry, &g_sta_retry_timer);

  const esp_timer_create_args_t test_timeout = {
      .callback              = [](void *) { end_test_failed("no answer from the network"); },
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "wifi_test",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&test_timeout, &g_test_timer);

  /* The switch away from a working network waits a moment, or the answer to
     the request that asked for it would never leave. */
  const esp_timer_create_args_t test_start = {
      .callback =
          [](void *) {
            ESP_LOGI(TAG, "Leaving \"%s\" to try \"%s\"", appcfg::wifi().ssid.c_str(), g_test.ssid.c_str());
            set_state(State::Connecting);
            esp_wifi_disconnect();
            wifi_config_t sta = {};
            std::memcpy(sta.sta.ssid, g_test.ssid.data(), g_test.ssid.size());
            std::memcpy(sta.sta.password, g_test_pass.data(), g_test_pass.size());
            esp_wifi_set_config(WIFI_IF_STA, &sta);
            esp_wifi_connect();
            esp_timer_start_once(g_test_timer, kTestTimeoutUs);
          },
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "wifi_test_go",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&test_start, &g_test_start_timer);

  const esp_timer_create_args_t commit = {
      .callback =
          [](void *) {
            appcfg::set_wifi(g_test.ssid, g_test_pass);
            ESP_LOGI(TAG, "Credentials for \"%s\" stored, switching over", g_test.ssid.c_str());
            wifi_config_t sta = {};
            std::memcpy(sta.sta.ssid, g_test.ssid.data(), g_test.ssid.size());
            std::memcpy(sta.sta.password, g_test_pass.data(), g_test_pass.size());
            g_test_pass.clear();
            g_retries = 0;
            set_state(State::Connecting);
            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &sta);
            esp_wifi_connect();
          },
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "wifi_commit",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&commit, &g_commit_timer);

  /* Full power unless something has been learned. Many configurations carry a
     reduced setting as a workaround for supplies that cannot deliver the
     transmit peak, but applying it pre-emptively costs range out of the meter
     pit to avoid a fault nobody here has observed -- and it would hide whether
     this supply is affected at all. A brownout is detected
     by name and steps this down; see diagnostics.

     Only after esp_wifi_start(): before it the driver answers
     ESP_ERR_WIFI_NOT_STARTED and the limit is silently lost. */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  ESP_ERROR_CHECK(esp_wifi_start());

  const int want = appcfg::tx_power_dbm();
  if (want > 0)
    apply_tx_power(want);

  /* The C6 speaks 802.11ax, but the driver logs
       "11ax/11ac mode can not work under phy bw 40M, phymode changed to 11N"
     and silently drops to WiFi 4, because 40 MHz is the default on 2.4 GHz and
     HE needs 20. Pinning the bandwidth gets WiFi 6 back. Throughput is
     irrelevant here -- a few hundred bytes a second -- but HE is markedly more
     airtime-efficient, which matters on a busy home network. */
  esp_err_t bw = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
  if (bw != ESP_OK)
    ESP_LOGW(TAG, "Could not pin 20 MHz bandwidth: %s", esp_err_to_name(bw));

  esp_err_t pr = esp_wifi_set_protocol(WIFI_IF_STA,
                                       WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
  if (pr != ESP_OK) {
    ESP_LOGW(TAG, "Could not enable 802.11ax: %s", esp_err_to_name(pr));
  } else {
    uint8_t active = 0;
    esp_wifi_get_protocol(WIFI_IF_STA, &active);
    ESP_LOGI(TAG, "Station protocols: 0x%02x (11ax %s)", active,
             (active & WIFI_PROTOCOL_11AX) ? "enabled" : "NOT enabled");
  }

  try_connect();
  return true;
}

State state() {
  return g_state;
}
std::string ip() {
  return g_ip;
}

std::string ssid() {
  if (g_state == State::ApMode)
    return appcfg::device().name;
  return appcfg::wifi().ssid;
}

int8_t rssi() {
  /* The driver now and then has no figure -- during a scan, or between two
     beacons -- and reports 0 dBm, which reads as a perfect signal. The last
     real one is the better answer while still connected. */
  static int8_t last = 0;
  if (g_state != State::Connected)
    return 0;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi < 0)
    last = ap.rssi;
  return last;
}

uint8_t channel() {
  uint8_t            primary = 0;
  wifi_second_chan_t second  = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK)
    return primary;
  return 0;
}

bool provisioning() {
  return g_state == State::ApMode;
}

bool scan(std::vector<Network> &out) {
  out.clear();
  if (g_testing || g_state == State::Connecting)
    return false;
  wifi_scan_config_t cfg = {};
  cfg.show_hidden        = false;
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK)
    return false;

  uint16_t                      n = 32;
  std::vector<wifi_ap_record_t> recs(n);
  if (esp_wifi_scan_get_ap_records(&n, recs.data()) != ESP_OK)
    return false;
  recs.resize(n);

  for (const auto &r : recs) {
    const std::string ssid(reinterpret_cast<const char *>(r.ssid));
    if (ssid.empty())
      continue;
    auto it = std::find_if(out.begin(), out.end(), [&](const Network &x) { return x.ssid == ssid; });
    if (it == out.end())
      out.push_back(Network{ssid, r.rssi, r.primary, r.authmode != WIFI_AUTH_OPEN});
    else if (r.rssi > it->rssi)
      *it = Network{ssid, r.rssi, r.primary, r.authmode != WIFI_AUTH_OPEN};
  }
  std::sort(out.begin(), out.end(), [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  return true;
}

TestStart test_credentials(const std::string &ssid, const std::string &password) {
  if (ssid.empty() || ssid.size() > kSsidMaxLen || password.size() > kPasswordMaxLen)
    return TestStart::Invalid;
  bool from_ap = false, from_sta = false;
  {
    std::lock_guard<std::mutex> l(g_test_mx);
    /* A probe in progress takes the station off its network: that is a
       running test, not a missing connection. */
    if (g_testing)
      return TestStart::Busy;
    /* A test has worked and is being applied: the device is about to switch
       for good (commit timer), or has joined with the setup network still
       lingering. A second request now would put its own SSID and password
       under the commit of the first -- stored untested. */
    if ((g_commit_timer && esp_timer_is_active(g_commit_timer)) || (g_ap_active && g_state == State::Connected))
      return TestStart::Settling;
    from_ap  = g_ap_active && g_state == State::ApMode;
    from_sta = !g_ap_active && g_state == State::Connected;
    if (!(from_ap || from_sta))
      return TestStart::Offline;
    g_testing = true;
    g_test    = Test{TestState::Testing, ssid, "", "", 0};
  }
  g_test_pass = password;

  if (from_sta) {
    ESP_LOGI(TAG, "Will try \"%s\" before storing it", ssid.c_str());
    esp_timer_start_once(g_test_start_timer, 500000);
    return TestStart::Started;
  }

  if (g_ap_retry_timer)
    esp_timer_stop(g_ap_retry_timer);

  ESP_LOGI(TAG, "Trying \"%s\" before storing it", ssid.c_str());
  esp_wifi_disconnect();
  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(sta.sta.password, password.data(), password.size());
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  esp_wifi_connect();
  esp_timer_start_once(g_test_timer, kTestTimeoutUs);
  return TestStart::Started;
}

Test test_status() {
  std::lock_guard<std::mutex> l(g_test_mx);
  Test                        t = g_test;
  if (t.state == TestState::Ok)
    t.restart_in_s = std::max<int64_t>(0, (g_linger_until_us - esp_timer_get_time()) / 1000000);
  return t;
}

const char *to_string(TestState s) {
  switch (s) {
    case TestState::Idle:
      return "idle";
    case TestState::Testing:
      return "testing";
    case TestState::Ok:
      return "ok";
    case TestState::Failed:
      return "failed";
  }
  return "?";
}

void set_low_latency(bool on) {
  esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
  ESP_LOGI(TAG, "Modem sleep %s", on ? "off (live stream)" : "on");
}

void apply_tx_power(int dbm) {
  const int d   = dbm > 0 ? dbm : appcfg::kTxPowerMaxDbm;
  esp_err_t err = esp_wifi_set_max_tx_power(static_cast<int8_t>(d * 4)); // API takes quarter dBm
  if (err == ESP_OK)
    ESP_LOGI(TAG, "Transmit power limited to %d dBm", d);
  else
    ESP_LOGW(TAG, "Transmit power %d dBm rejected: %s", d, esp_err_to_name(err));
}

void apply_clock() {
  const appcfg::Clock c = appcfg::clock();
  setenv("TZ", c.tz_posix.c_str(), 1);
  tzset();
  ESP_LOGI(TAG, "Timezone %s (%s)", c.tz_name.c_str(), c.tz_posix.c_str());
  /* A new server only matters once SNTP runs; before that it is picked up on
     the first address. */
  std::lock_guard<std::mutex> lock(g_sntp_mx);
  const std::string           own = c.server.empty() ? kFallbackA : c.server;
  if (g_sntp_started && own == g_ntp[0])
    return; // only the timezone changed
  if (g_sntp_started) {
    esp_netif_sntp_deinit();
    g_sntp_started = false;
  }
  if (g_sntp_wanted)
    start_sntp_locked();
}

Time time_info() {
  Time t;
  t.synced = g_time_synced;
  if (!t.synced)
    return t;

  /* Which server answered: bit 0 of the reachability register is the latest
     poll, so a server that answered just now beats one that answered before. */
  {
    std::lock_guard<std::mutex> lock(g_sntp_mx);
    if (g_sntp_started) {
      int best = -1;
      for (int i = 0; i < 2; i++) {
        const uint8_t r = esp_sntp_getreachability(i);
        if ((r & 1) && (best < 0 || !(esp_sntp_getreachability(best) & 1)))
          best = i;
        else if (r && best < 0)
          best = i;
      }
      if (best >= 0)
        t.server = g_ntp[best];
    }
  }

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  t.epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  /* Both clocks are read as close together as they can be, so the difference is
     the offset between them and not a measurement of how long this took. */
  t.boot_epoch_ms = t.epoch_ms - esp_timer_get_time() / 1000;
  return t;
}

} // namespace appnet
