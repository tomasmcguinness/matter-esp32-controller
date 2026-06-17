#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t matter_controller_start(void);
uint64_t  matter_controller_allocate_node_id(void);
esp_err_t matter_controller_commission_on_network(const char *onboarding_payload, uint64_t *node_id_out);
esp_err_t matter_controller_commission_ble_wifi(const char *onboarding_payload, uint64_t *node_id_out);
esp_err_t matter_controller_remove_node(uint64_t node_id);
esp_err_t matter_controller_interrogate_node(uint64_t node_id);
// Read/write the OnOff state of an On/Off-capable device (cluster 0x0006).
esp_err_t matter_controller_get_onoff(uint64_t node_id, bool *on_out);
esp_err_t matter_controller_set_onoff(uint64_t node_id, bool on);
// Create/remove a Matter binding so a switch directly controls a light's OnOff cluster.
// Writes the light's ACL (granting the switch Operate) and the switch's Binding attribute.
// Pass 0 for an endpoint to auto-resolve it from the device manager.
esp_err_t matter_controller_create_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                           uint64_t light_node_id, uint16_t light_endpoint);
esp_err_t matter_controller_delete_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                           uint64_t light_node_id, uint16_t light_endpoint);
esp_err_t matter_factory_reset(void);

#ifdef __cplusplus
}
#endif
