/*
   Meter reading and flow to an MQTT broker.

   The web interface answers "what is the meter doing right now" for someone
   looking at it. This answers the same question for something that is not:
   readings are retained, so a broker restart or a newly started consumer sees
   the current state immediately rather than waiting for the next litre to
   flow -- which, overnight, could be hours.

   One availability topic. MQTT allows a single last will per connection, so a
   second "sensor lost" topic could never be corrected by the broker when the
   device died. What a subscriber actually wants is one answer: is what I am
   reading current? The topic says offline both when the device is gone and
   when the sensor delivers no plausible signal.

   It is republished on a slow cycle as well as on change, so a household using
   no water can be told apart from a vanished device.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace appmqtt {

/* What a subscriber needs: which reading, what it says, and whether it still
   means anything -- plus what it is called and what it measures, which is what
   the discovery announcement is built from. The metadata comes from the
   application and is carried through untouched; nothing here decides what a
   value means. */
struct Reading {
  std::string id;
  std::string name;
  std::string kind;
  std::string value;
  std::string unit;
  std::string device_class;
  std::string state_class;
  std::string entity_category;
  std::string icon;
  bool        available = false;
  /* The value is a JSON object: Home Assistant shows `json_state` from it as
     the state and every field as an attribute -- one entity for a set of
     diagnostics instead of one per number. */
  std::string json_state;
  /* Seconds after which Home Assistant marks the entity unavailable without a
     new value; 0 = never. For entities that ignore the sensor's availability. */
  int expire_after_s = 0;
  /* Published on <root>/<topic> instead of <root>/state/<id>. */
  std::string topic;
};

/* The adapter itself, as Home Assistant should show it: one device, with every
   reading as an entity under it. */
struct Description {
  /* Identity comes from the MAC, never from the name the user picked. Home
     Assistant keys a device on this string, so deriving it from an editable
     name would mean renaming the adapter silently orphaned every entity and
     created a duplicate device beside it. The chosen name is what gets
     displayed -- it just does not decide identity. */
  std::string device_id;
  std::string device_name;
  std::string model;
  std::string manufacturer;
  std::string firmware;
  /* Ids of plain sensors earlier firmware announced and this one no longer
     does. Their discovery topics are retained in the broker and would keep
     the entities in Home Assistant forever; on connect they are cleared. */
  std::vector<std::string> retired;
  /* State topics, below the root ("" = the root itself), that earlier firmware
     published retained and this one no longer does; cleared on connect. */
  std::vector<std::string> retired_topics;
};

bool begin();

/* Called when the station comes up or goes away. The client is only started
   once there is a network, and stopped when there is not -- an MQTT client
   retrying against no route is just noise. */
void on_network(bool connected);

bool connected();
bool enabled();

/* Reports the current configuration back for the settings page. Never returns
   the password. */
std::string status_json();

/* The set of readings to publish, replacing whatever was set before. When the
   set shrinks, entities disappear: a discovery topic that is no longer wanted is cleared here, or Home Assistant
   would keep showing an entity nothing will ever update again.

   The state topics are left alone. Their retained payload is a value that was
   true when it was written, and clearing it would not make a subscriber better
   informed than an old reading does. */
void announce(const Description &device, const Reading *readings, size_t count);

/* One reading changed. Retained, so a consumer that connects later still sees
   it; an unavailable reading publishes an empty payload, so the meter's own
   readings are always passed as available. */
void publish(const Reading &r);

/* Whether the register delivers a usable signal at all -- separate from
   whether this device is reachable. A sensor that has come loose otherwise
   looks like a household that uses no water. */
void set_meter_online(bool online);

/* Applies newly stored settings without a restart. */
void reconfigure();

} // namespace appmqtt
