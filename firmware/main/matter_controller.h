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
esp_err_t matter_factory_reset(void);

#ifdef __cplusplus
}
#endif
