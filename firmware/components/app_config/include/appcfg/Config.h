/*
   Persistent configuration in NVS.

   Everything the user can change at runtime lives here, so the firmware never
   has to be rebuilt to move to a different network or change a password.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>
#include <string>

namespace appcfg {

struct Wifi {
  std::string ssid;
  std::string password;
  bool        configured() const {
    return !ssid.empty();
  }
};

struct Auth {
  /* Off by default: a fresh device must be reachable to be set up at all, and
     the exposure is reading a water meter. The user turns it on and picks the
     password. */
  bool        enabled = false;
  std::string user    = "admin";
  std::string salt_hex;
  std::string hash_hex;
  bool        has_password() const {
    return !hash_hex.empty();
  }
};

/* Where decoded readings go, if anywhere.

   Off until a broker is entered, because the alternative -- guessing a hostname
   and retrying forever -- fills the log of a device nobody is watching. The
   topic root is derived from the device name when left blank, so two adapters
   in one house do not collide by default. */
struct Mqtt {
  bool enabled = false;

  /* Kept apart rather than as one URI string: a scheme, a host and a port are
     three different questions, and typing them into one field made two of them
     easy to get silently wrong. The URI is assembled on the way to the client. */
  bool        tls = false;
  std::string host;
  uint16_t    port = kPortPlain;

  /* Empty: the device name is used, which is what the broker saw before this
     was settable. */
  std::string client_id;

  /* Credentials are a deliberate choice, not an inference from a filled-in
     field: with this off the client connects anonymously and anything stored is
     cleared, so a half-typed user name cannot lock the device out of a broker
     that accepts anonymous clients. */
  bool        auth = false;
  std::string user;
  std::string password;

  /* Only meaningful with tls. Without a CA the connection is not verified at
     all, so the two are one choice: bring a certificate, or say plainly that
     the server is not being checked. */
  bool        tls_insecure = false;
  std::string ca_cert; // PEM

  std::string base; // empty: derive from the device name

  /* Announce the entities to Home Assistant, so it builds the device itself
   instead of needing a hand-written sensor for every reading. On by default:
   a broker in a house almost always has Home Assistant behind it, and a
   device that publishes values nothing displays is a puzzle rather than a
   feature. */
  bool discovery = true;
  /* Where Home Assistant listens for those announcements. Configurable because
     it is configurable there. */
  std::string discovery_prefix = "homeassistant";

  static constexpr uint16_t kPortPlain = 1883;
  static constexpr uint16_t kPortTls   = 8883;

  bool configured() const {
    return !host.empty();
  }
  /* mqtt://host:1883 -- what the client actually connects to. */
  std::string uri() const;
};

struct Device {
  /* Also used as the mDNS hostname and the MQTT client id. */
  std::string name;
};

/* Time server and timezone.

   The device counts in UTC; the timezone is for showing times, in the web
   interface and the log. Both are kept as the interface picked them: the IANA
   name for the browser, the POSIX rule for the C library, which knows no
   names. */
struct Clock {
  std::string server   = kDefaultServer; // empty: the built-in servers only
  std::string tz_name  = "Europe/Berlin";
  std::string tz_posix = "CET-1CEST,M3.5.0,M10.5.0/3";

  static constexpr const char *kDefaultServer = "pool.ntp.org";
};

bool begin();

Wifi wifi();
bool set_wifi(const std::string &ssid, const std::string &password);
bool clear_wifi();

Auth auth();
/* Stores a PBKDF2-HMAC-SHA256 digest with a fresh random salt. An empty
   password clears the stored credential and forces protection off. */
bool set_password(const std::string &user, const std::string &password);
bool set_auth_enabled(bool enabled);
bool verify_password(const std::string &user, const std::string &password);

Mqtt mqtt();
bool set_mqtt(const Mqtt &m);

/* The topic root actually in use: the configured one, or one derived from the
   device name when it is blank. */
std::string mqtt_base();

/* The client id actually in use: the configured one, or the device name when it
   is blank -- which is what a broker saw before this was settable. */
std::string mqtt_client_id();

Device device();
bool   set_device_name(const std::string &name);

/* Time server and timezone, see Clock. */
Clock clock();
bool  set_clock(const Clock &c);

/* Set by an update uploaded from the web interface: the page will confirm
   the new image itself once it is back, so the image waits for that as well
   as for its self-test (see probation.h). Cleared once it is decided. */
bool ota_confirm_by_ui();
bool set_ota_confirm_by_ui(bool on);

/* WiFi transmit power in dBm. 0 means "use the maximum".

   Not capped by default: the device has to stay reachable from a meter pit,
   often in a basement, and reducing power to guard against a brownout nobody
   has observed trades a certain cost for an uncertain benefit. Brownouts are
   detected by name, so the firmware can step this down when one actually
   happens instead of assuming it will. */
int  tx_power_dbm();
bool set_tx_power_dbm(int dbm);

/* The valid fixed values. Below this the signal barely makes it out of the
   meter pit; above it the driver refuses anyway. */
inline constexpr int kTxPowerMinDbm = 2;
inline constexpr int kTxPowerMaxDbm = 20;

/* "flowtick-a1b2c3" from the WiFi MAC -- the default device name. */
std::string default_device_name();

/* "water-meter-a1b2c3" from the WiFi MAC -- the Home Assistant device
   identity. From the MAC and not from the name the user picked: that one is
   editable, and renaming it would orphan every published entity. And still
   under the project's former name, for the same reason: changing it would
   leave every existing device behind as a dead twin. */
std::string device_id();

/* Erases every setting this component owns: network, credentials, device name.
   Distinct from the BOOT-button hold, which only clears what can lock you out
   and deliberately keeps the rest. */
bool factory_reset();

} // namespace appcfg
