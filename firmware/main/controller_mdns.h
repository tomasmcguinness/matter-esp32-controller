#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialises mDNS and advertises the controller under the delegated hostname
// "matter-controller.local". The delegated host's address tracks the Ethernet
// interface's IP, (re)published whenever it acquires or renews a lease.
// Safe to call after esp_event_loop_create_default() and esp_netif_init();
// mDNS may already have been started by the Matter stack, which is handled.
esp_err_t controller_mdns_start(void);

#ifdef __cplusplus
}
#endif
