/*
   Persistent configuration in NVS.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#include "appcfg/Config.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <psa/crypto.h>

#include <cstdio>
#include <cstring>

namespace appcfg {
namespace {

const char *const TAG = "appcfg";

const char *const kNsWifi = "wifi";
const char *const kNsAuth = "auth";
const char *const kNsDev  = "device";
const char *const kNsMqtt = "mqtt";

/* Deliberately on the slow side for an embedded chip: the cost is what makes a
   weak password survive someone reading the flash.

   It is affordable only because the web layer caches a verified Authorization
   header rather than deriving the key again for every request. Basic
   authentication resends the header constantly, so without that cache this
   would run several times a minute, in the task that also serves the monitor. */
constexpr int    kPbkdf2Iterations = 20000;
constexpr size_t kSaltLen          = 16;
constexpr size_t kHashLen          = 32;

std::string to_hex(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(len * 2);
  char buf[3];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02x", data[i]);
    out += buf;
  }
  return out;
}

bool from_hex(const std::string &hex, uint8_t *out, size_t out_len) {
  if (hex.size() != out_len * 2)
    return false;
  for (size_t i = 0; i < out_len; i++) {
    unsigned v;
    if (sscanf(hex.c_str() + i * 2, "%2x", &v) != 1)
      return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

/* ESP-IDF 6 ships mbedtls 4, where the standalone crypto primitives are gone
   and everything goes through PSA Crypto. mbedtls_pkcs5_pbkdf2_hmac_ext() and
   even mbedtls/sha256.h no longer exist. */
bool derive(const std::string &password, const uint8_t *salt, uint8_t *out) {
  psa_status_t rc = psa_crypto_init();
  if (rc != PSA_SUCCESS) {
    ESP_LOGE(TAG, "psa_crypto_init failed: %d", static_cast<int>(rc));
    return false;
  }

  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  bool                           ok = false;
  do {
    rc = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (rc != PSA_SUCCESS)
      break;
    rc = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, kPbkdf2Iterations);
    if (rc != PSA_SUCCESS)
      break;
    rc = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, kSaltLen);
    if (rc != PSA_SUCCESS)
      break;
    rc = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                        reinterpret_cast<const uint8_t *>(password.data()), password.size());
    if (rc != PSA_SUCCESS)
      break;
    rc = psa_key_derivation_output_bytes(&op, out, kHashLen);
    if (rc != PSA_SUCCESS)
      break;
    ok = true;
  } while (false);

  psa_key_derivation_abort(&op);
  if (!ok)
    ESP_LOGE(TAG, "PBKDF2 derivation failed: %d", static_cast<int>(rc));
  return ok;
}

/* Comparison that does not leak how many leading bytes matched. */
bool equal_ct(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); i++)
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

std::string get_str(const char *ns, const char *key, const std::string &fallback = "") {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return fallback;
  size_t      len = 0;
  std::string out = fallback;
  if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0) {
    out.resize(len);
    if (nvs_get_str(h, key, out.data(), &len) == ESP_OK)
      out.resize(len ? len - 1 : 0); // drop the trailing NUL
    else
      out = fallback;
  }
  nvs_close(h);
  return out;
}

bool set_str(const char *ns, const char *key, const std::string &value) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_str(h, key, value.c_str());
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

uint8_t get_u8(const char *ns, const char *key, uint8_t fallback) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return fallback;
  uint8_t v = fallback;
  nvs_get_u8(h, key, &v);
  nvs_close(h);
  return v;
}

bool set_u8(const char *ns, const char *key, uint8_t value) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_u8(h, key, value);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

uint16_t get_u16(const char *ns, const char *key, uint16_t fallback) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return fallback;
  uint16_t v = fallback;
  nvs_get_u16(h, key, &v);
  nvs_close(h);
  return v;
}

bool set_u16(const char *ns, const char *key, uint16_t value) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_u16(h, key, value);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

} // namespace

bool begin() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs erasing (%s), reinitialising", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

namespace {
std::string with_mac(const char *prefix) {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s-%02x%02x%02x", prefix, mac[3], mac[4], mac[5]);
  return buf;
}
} // namespace

std::string default_device_name() {
  return with_mac("flowtick");
}

std::string device_id() {
  return with_mac("water-meter");
}

Wifi wifi() {
  Wifi w;
  w.ssid     = get_str(kNsWifi, "ssid");
  w.password = get_str(kNsWifi, "pass");
  return w;
}

bool set_wifi(const std::string &ssid, const std::string &password) {
  return set_str(kNsWifi, "ssid", ssid) && set_str(kNsWifi, "pass", password);
}

bool clear_wifi() {
  return set_wifi("", "");
}

/* User, salt and hash live in one entry, "salt:hash:user". NVS writes a
   single entry atomically; as three, a power cut between salt and hash left a
   pair that matches no password and locked the interface until the BOOT
   button was held. The user goes last, so it may contain a colon. */
constexpr const char *kCredMissing = "\x01";

void erase_key(const char *ns, const char *key) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return;
  if (nvs_erase_key(h, key) == ESP_OK)
    nvs_commit(h);
  nvs_close(h);
}

bool store_cred(const std::string &user, const std::string &salt_hex, const std::string &hash_hex) {
  const std::string v = salt_hex.empty() ? std::string() : salt_hex + ":" + hash_hex + ":" + user;
  if (!set_str(kNsAuth, "cred", v))
    return false;
  /* The three separate keys of earlier firmware. Only after the new entry is
     in place, and it takes precedence, so a cut here costs nothing. */
  for (const char *k : {"user", "salt", "hash"})
    erase_key(kNsAuth, k);
  return true;
}

Auth auth() {
  Auth a;
  a.enabled              = get_u8(kNsAuth, "enabled", 0) != 0;
  const std::string cred = get_str(kNsAuth, "cred", kCredMissing);
  if (cred == kCredMissing) {
    a.user     = get_str(kNsAuth, "user", "admin");
    a.salt_hex = get_str(kNsAuth, "salt");
    a.hash_hex = get_str(kNsAuth, "hash");
  } else {
    const size_t c1 = cred.find(':'), c2 = c1 == std::string::npos ? c1 : cred.find(':', c1 + 1);
    if (c2 != std::string::npos) {
      a.salt_hex = cred.substr(0, c1);
      a.hash_hex = cred.substr(c1 + 1, c2 - c1 - 1);
      a.user     = cred.substr(c2 + 1);
    }
  }
  /* Protection without a stored password would lock everyone out with no way
     back, so it can never be on in that state. */
  if (!a.has_password())
    a.enabled = false;
  return a;
}

bool set_password(const std::string &user, const std::string &password) {
  if (password.empty()) {
    ESP_LOGI(TAG, "Password cleared, protection off");
    return store_cred("", "", "") && set_u8(kNsAuth, "enabled", 0);
  }

  uint8_t salt[kSaltLen];
  esp_fill_random(salt, sizeof(salt));

  uint8_t hash[kHashLen];
  if (!derive(password, salt, hash))
    return false;

  return store_cred(user.empty() ? "admin" : user, to_hex(salt, kSaltLen), to_hex(hash, kHashLen));
}

bool set_auth_enabled(bool enabled) {
  if (enabled && !auth().has_password()) {
    ESP_LOGW(TAG, "Refusing to enable protection without a password");
    return false;
  }
  return set_u8(kNsAuth, "enabled", enabled ? 1 : 0);
}

bool verify_password(const std::string &user, const std::string &password) {
  Auth a = auth();
  if (!a.has_password())
    return false;
  if (a.user != user)
    return false;

  uint8_t salt[kSaltLen];
  if (!from_hex(a.salt_hex, salt, kSaltLen))
    return false;

  uint8_t hash[kHashLen];
  if (!derive(password, salt, hash))
    return false;

  return equal_ct(to_hex(hash, kHashLen), a.hash_hex);
}

std::string Mqtt::uri() const {
  char buf[24];
  snprintf(buf, sizeof(buf), ":%u", static_cast<unsigned>(port));
  return (tls ? "mqtts://" : "mqtt://") + host + buf;
}

/* Settings written before the broker was split into scheme, host and port are
   still a single "mqtt://host:1883" string. Reading them back rather than
   dropping them means an upgrade does not quietly disconnect a working setup;
   the next save writes the parts and the old key stops being consulted. */
void adopt_legacy_uri(Mqtt &m) {
  const std::string uri = get_str(kNsMqtt, "uri");
  if (uri.empty())
    return;

  std::string  rest = uri;
  const size_t sep  = rest.find("://");
  if (sep != std::string::npos) {
    m.tls = rest.compare(0, sep, "mqtts") == 0 || rest.compare(0, sep, "wss") == 0;
    rest  = rest.substr(sep + 3);
  }
  /* A path would only ever be a websocket one, which this never supported. */
  const size_t slash = rest.find('/');
  if (slash != std::string::npos)
    rest = rest.substr(0, slash);

  const size_t colon = rest.rfind(':');
  if (colon != std::string::npos) {
    m.port = static_cast<uint16_t>(atoi(rest.c_str() + colon + 1));
    rest   = rest.substr(0, colon);
  } else {
    m.port = m.tls ? Mqtt::kPortTls : Mqtt::kPortPlain;
  }
  m.host = rest;
  if (m.port == 0)
    m.port = m.tls ? Mqtt::kPortTls : Mqtt::kPortPlain;
}

Mqtt mqtt() {
  Mqtt m;
  m.enabled = get_u8(kNsMqtt, "on", 0) != 0;
  m.tls     = get_u8(kNsMqtt, "tls", 0) != 0;
  m.host    = get_str(kNsMqtt, "host");
  m.port    = get_u16(kNsMqtt, "port", Mqtt::kPortPlain);
  if (m.host.empty())
    adopt_legacy_uri(m);

  m.client_id        = get_str(kNsMqtt, "cid");
  m.auth             = get_u8(kNsMqtt, "auth", 0) != 0;
  m.user             = get_str(kNsMqtt, "user");
  m.password         = get_str(kNsMqtt, "pass");
  m.tls_insecure     = get_u8(kNsMqtt, "tlsskip", 0) != 0;
  m.ca_cert          = get_str(kNsMqtt, "ca");
  m.base             = get_str(kNsMqtt, "base");
  m.discovery        = get_u8(kNsMqtt, "disc", 1) != 0;
  m.discovery_prefix = get_str(kNsMqtt, "dpfx", "homeassistant");
  return m;
}

bool set_mqtt(const Mqtt &m) {
  bool ok = set_u8(kNsMqtt, "tls", m.tls ? 1 : 0);
  ok      = set_str(kNsMqtt, "host", m.host) && ok;
  ok      = set_u16(kNsMqtt, "port", m.port ? m.port : (m.tls ? Mqtt::kPortTls : Mqtt::kPortPlain)) && ok;
  ok      = set_str(kNsMqtt, "cid", m.client_id) && ok;
  ok      = set_u8(kNsMqtt, "auth", m.auth ? 1 : 0) && ok;
  /* Credentials only exist while they are switched on. Keeping them behind a
     disabled toggle would mean the stored password outlives the decision to
     stop using it, which is not what turning something off looks like. */
  ok = set_str(kNsMqtt, "user", m.auth ? m.user : std::string()) && ok;
  ok = set_str(kNsMqtt, "pass", m.auth ? m.password : std::string()) && ok;
  ok = set_u8(kNsMqtt, "tlsskip", m.tls_insecure ? 1 : 0) && ok;
  /* Same reasoning for the certificate: it belongs to a verified TLS
     connection, so it goes when there is none. */
  ok = set_str(kNsMqtt, "ca", (m.tls && !m.tls_insecure) ? m.ca_cert : std::string()) && ok;
  /* The split fields are the truth now; the old combined key is cleared so a
     later read cannot resurrect a stale broker through the migration path. */
  ok = set_str(kNsMqtt, "uri", std::string()) && ok;
  ok = set_str(kNsMqtt, "base", m.base) && ok;
  ok = set_u8(kNsMqtt, "disc", m.discovery ? 1 : 0) && ok;
  ok = set_str(kNsMqtt, "dpfx", m.discovery_prefix) && ok;
  /* Enabled last, and only with somewhere to connect to. Turning it on without
     a broker would leave the client retrying an empty address forever. */
  ok = set_u8(kNsMqtt, "on", (m.enabled && m.configured()) ? 1 : 0) && ok;
  return ok;
}

/* Slashes and spaces would split or break a topic, so the device name is
   reduced to what is safe in one. */
std::string mqtt_client_id() {
  const Mqtt m = mqtt();
  if (!m.client_id.empty())
    return m.client_id;
  const std::string name = device().name;
  return name.empty() ? default_device_name() : name;
}

std::string mqtt_base() {
  const Mqtt m = mqtt();
  if (!m.base.empty())
    return m.base;

  std::string name = device().name;
  std::string safe;
  for (char c : name) {
    if (c == '/' || c == '+' || c == '#' || c == ' ')
      safe += '-';
    else
      safe += c;
  }
  if (safe.empty())
    safe = "flowtick";
  return "flowtick/" + safe;
}

Device device() {
  Device d;
  d.name = get_str(kNsDev, "name");
  if (d.name.empty())
    d.name = default_device_name();
  return d;
}

bool set_device_name(const std::string &name) {
  return set_str(kNsDev, "name", name);
}

Clock clock() {
  Clock c;
  c.server   = get_str(kNsDev, "ntp", c.server);
  c.tz_name  = get_str(kNsDev, "tz", c.tz_name);
  c.tz_posix = get_str(kNsDev, "tzposix", c.tz_posix);
  return c;
}

bool set_clock(const Clock &c) {
  /* An empty server is stored as such -- it means "built-in only" and must not
     read back as the default. get_str only falls back on a missing key. */
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_str(h, "ntp", c.server.c_str());
  if (err == ESP_OK)
    err = nvs_set_str(h, "tz", c.tz_name.c_str());
  if (err == ESP_OK)
    err = nvs_set_str(h, "tzposix", c.tz_posix.c_str());
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool ota_confirm_by_ui() {
  return get_u8(kNsDev, "ota_ui", 0) != 0;
}

bool set_ota_confirm_by_ui(bool on) {
  return set_u8(kNsDev, "ota_ui", on ? 1 : 0);
}

int tx_power_dbm() {
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READONLY, &h) != ESP_OK)
    return 0;
  int8_t v = 0;
  nvs_get_i8(h, "txpower", &v);
  nvs_close(h);
  return v;
}

bool set_tx_power_dbm(int dbm) {
  nvs_handle_t h;
  if (nvs_open(kNsDev, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_i8(h, "txpower", static_cast<int8_t>(dbm));
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool factory_reset() {
  ESP_LOGW(TAG, "Factory reset: erasing all stored settings");
  bool ok = true;
  for (const char *ns : {kNsWifi, kNsAuth, kNsDev, kNsMqtt}) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
      continue;
    /* Erase the namespace wholesale rather than key by key, so a key added
       later is not silently left behind by a reset that predates it. */
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK)
      err = nvs_commit(h);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Could not erase namespace \"%s\": %s", ns, esp_err_to_name(err));
      ok = false;
    }
    nvs_close(h);
  }
  return ok;
}

} // namespace appcfg
