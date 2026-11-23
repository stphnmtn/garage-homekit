#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*net_cb_t)(bool up);

// Starts Ethernet with DHCP; `cb` runs on every IP gained (true) or link lost (false).
void net_init(const char *hostname, net_cb_t cb);
// Base MAC (also the Ethernet MAC), for the accessory serial number.
void net_get_mac(uint8_t mac[6]);
