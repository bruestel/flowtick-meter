/*
   WiFi with an access-point fallback.

   The device ends up at a meter where no cable may be attached, so it must
   always remain reachable somehow. If a station connection is not
   configured or cannot be established, it opens its own network and serves a
   captive portal, which is the only way back in.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace appnet {

enum class State : uint8_t {
  Idle,
  Connecting,
  Connected,
  ApMode, // own network, captive portal, setup allowed without a password
};

const char *to_string(State s);

using StateCallback = std::function<void(State)>;

bool begin(StateCallback on_state_change);

State       state();
std::string ip();
std::string ssid();
int8_t      rssi();

/* The channel the station is using. Both radios share one, so this is what
   decides whether a setup access point can stay up alongside it. */
uint8_t channel();

/* True while the device is only reachable through its own access point. Setup
   endpoints must stay open in that case -- requiring a password here would lock
   the user out of a device they cannot reach any other way. */
bool provisioning();

/* Set the transmit power immediately. 0 means maximum. Stores nothing -- that
   is appcfg::set_tx_power_dbm()'s job. */
void apply_tx_power(int dbm);

/* WiFi modem sleep off while something needs a steady stream (the live
   scope): with it the radio sleeps between beacons and packets arrive in
   bursts with pauses of up to a second. On again when done. */
void set_low_latency(bool on);

/* One network in range. Several access points with the same name are one
   entry, the strongest. */
struct Network {
  std::string ssid;
  int8_t      rssi    = 0;
  uint8_t     channel = 0;
  bool        secure  = false;
};

/* Blocking, roughly two seconds. Fails while a join is in progress -- the
   radio cannot scan and associate at the same time. */
bool scan(std::vector<Network> &out);

/* Joining a network: the credentials are tried first and stored only once
   they have produced an address, so a mistyped password ends in "wrong
   password" instead of a device that has locked itself out.

   From the setup network: on success it stays up for 30 s, so whoever is on
   it can read the new address and the page can move over to it; then the
   device restarts. It restarts rather than just closing the access point
   because an in-place switch measured as unreliable in the bsh-dbus-idf
   project this code comes from: the access point holds the radio on its
   channel, and the station has to follow the router's.

   From a working network: the device leaves it half a second later (so the
   answer to this request still goes out) and tries the new one. Either way
   it then goes back, so the page that asked can read the outcome -- on
   success including the address on the new network, which may be in another
   subnet the page could never have guessed. After a success it switches over
   for good 12 s later; restart_in_s counts down to that.

   Only in access-point mode or while connected; the result says why not
   otherwise. */
/* What a WiFi SSID and password may be, as 802.11 and the driver allow. */
inline constexpr size_t kSsidMaxLen     = 32;
inline constexpr size_t kPasswordMaxLen = 64;

enum class TestStart : uint8_t {
  Started,
  Busy,     // a test is already running
  Settling, // a successful test is still being applied
  Invalid,  // SSID or password too long, or empty SSID
  Offline,  // neither on the setup network nor connected
};
TestStart test_credentials(const std::string &ssid, const std::string &password);

enum class TestState : uint8_t {
  Idle,
  Testing,
  Ok,
  Failed
};
struct Test {
  TestState   state = TestState::Idle;
  std::string ssid;
  std::string error;            // on Failed
  std::string ip;               // on Ok: the address on the new network
  int64_t     restart_in_s = 0; // on Ok: until the setup network closes / the switch
};
Test        test_status();
const char *to_string(TestState s);

/* Wall-clock time, once SNTP has managed to set it.

   The device keeps UTC and carries no timezone. Frames stay stamped with the
   monotonic uptime clock, because that is what keeps the spacing between them
   exact -- an NTP correction steps the wall clock, and must never make two
   frames look as though they arrived out of order. `boot_epoch_ms` is the
   bridge: add a frame's uptime stamp to it and the result is the absolute time
   it arrived. It is derived on every call, so a later correction applies to
   frames already captured rather than only to new ones.

   The timezone from the settings is for display only (see apply_clock);
   everything the device hands out stays in UTC. */
struct Time {
  bool        synced        = false;
  int64_t     epoch_ms      = 0; // now, milliseconds since the Unix epoch, UTC
  int64_t     boot_epoch_ms = 0; // epoch time at which the uptime clock read zero
  std::string server;            // the time server that answered last
};
Time time_info();

/* Takes the time server and timezone from the settings: sets TZ, and restarts
   SNTP if it is already running. Call at boot and after changing them. */
void apply_clock();

} // namespace appnet
