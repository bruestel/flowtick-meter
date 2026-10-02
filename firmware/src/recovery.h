/*
   Recovery hold on the BOOT button.

   The two automatic paths back into the device -- no credentials stored, and
   repeated connection failures -- both assume the join does not succeed. They
   do nothing about the case where the device happily joins a network you cannot
   reach: a router that changed subnets, a guest VLAN, an SSID that turned out to
   be someone else's. Then there is no way in at all.

   This is that way in. It needs no network, no cable and no browser -- only
   physical access, which is exactly what someone standing at the meter has.

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>

namespace recovery {

void begin();

/* Poll from the main loop. Holding the button for kHoldUs erases the stored
   WiFi credentials and reboots, which brings the setup access point back. */
void tick(int64_t now_us);

} // namespace recovery
