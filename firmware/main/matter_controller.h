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
// Allocates the next free Matter group id (and matching keyset id) on this
// controller, starting at 1. Persisted in NVS.
uint16_t  matter_controller_allocate_group_id(void);
esp_err_t matter_controller_commission_on_network(const char *onboarding_payload, uint64_t *node_id_out);
esp_err_t matter_controller_commission_ble_wifi(const char *onboarding_payload, uint64_t *node_id_out);
esp_err_t matter_controller_remove_node(uint64_t node_id);
esp_err_t matter_controller_interrogate_node(uint64_t node_id);
// Read/write the OnOff state of an On/Off-capable device (cluster 0x0006).
esp_err_t matter_controller_get_onoff(uint64_t node_id, bool *on_out);
esp_err_t matter_controller_set_onoff(uint64_t node_id, bool on);
// Make a device blink/beep via the Identify cluster (0x0003) for 15 seconds.
esp_err_t matter_controller_identify(uint64_t node_id);
// Read a device's Access Control List (cluster 0x001F). On success *json_out is set
// to a malloc'd JSON string { "entries": [ { "privilege", "authMode", "subjects":[..] } ] }
// that the caller must free(). Subjects are JSON strings (64-bit node ids).
esp_err_t matter_controller_get_acl(uint64_t node_id, char **json_out);
// Read a device's Binding table (cluster 0x001E) from its switch endpoint. On success
// *json_out is set to a malloc'd JSON string { "endpoint": <ep|null>, "entries":
// [ { "node":"..", "endpoint", "cluster" } ] } that the caller must free(). Devices
// with no switch endpoint return an empty entries list.
esp_err_t matter_controller_get_binding_table(uint64_t node_id, char **json_out);
// Create/remove a Matter binding so a switch directly controls a light's OnOff cluster.
// Writes the light's ACL (granting the switch Operate) and the switch's Binding attribute.
// Pass 0 for an endpoint to auto-resolve it from the device manager.
esp_err_t matter_controller_create_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                           uint64_t light_node_id, uint16_t light_endpoint);
esp_err_t matter_controller_delete_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                           uint64_t light_node_id, uint16_t light_endpoint);

// ---------------------------------------------------------------------------
// Matter groups (Group Key Management 0x003F + Groups 0x0004)
// ---------------------------------------------------------------------------
// All groups on this controller share ONE application keyset (the IPK is keyset
// 0; we use a single app keyset for every group). Devices have a tiny group-key
// table per fabric (MaxGroupKeysPerFabric, often 3), so a keyset-per-group quickly
// returns RESOURCE_EXHAUSTED — sharing keeps the table to IPK + one app key.

// Create a group on the controller's own fabric: lazily provisions the shared
// epoch key (random 16 bytes, persisted in NVS, registered with the local
// GroupDataProvider on first use) and binds it to the group so the controller can
// also send groupcast.
esp_err_t matter_controller_create_group(uint16_t group_id, const char *name);
// Tear down a group locally (unbinds the shared keyset from the group + removes the
// group info). The shared keyset is left intact for the other groups.
esp_err_t matter_controller_delete_group(uint16_t group_id);
// Add a device as a group member: installs the shared group key (KeySetWrite), maps
// the group to it (GroupKeyMap), grants the group Operate on the device's ACL, and
// issues AddGroup on the device's application endpoint.
esp_err_t matter_controller_add_group_member(uint64_t node_id, uint16_t group_id, const char *name);
// Reverse of add_group_member: RemoveGroup, drop the GroupKeyMap entry and the
// group's ACL Operate subject. Best-effort.
esp_err_t matter_controller_remove_group_member(uint64_t node_id, uint16_t group_id);
// Bind a switch to a whole group: installs the shared group key on the switch and
// adds a group target ({Group, OnOff cluster}) to its Binding table so the switch
// drives every group member with one groupcast command.
esp_err_t matter_controller_create_group_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                                 uint16_t group_id);
esp_err_t matter_controller_delete_group_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                                 uint16_t group_id);
// Clear all group state this controller provisioned on a device: empties its
// GroupKeyMap, removes the app keysets (KeySetRemove; the IPK keyset 0 is left
// intact), and RemoveAllGroups on its application endpoint. Used to recover a
// device whose group-key table filled up from earlier keyset-per-group attempts.
esp_err_t matter_controller_reset_node_groups(uint64_t node_id);
// Reset a device's Access Control List to just the controller's Administer/CASE
// entry, dropping every other entry (stale Operate/CASE bindings, group entries,
// and any corrupt privilege-0 junk). The controller can never lock itself out.
esp_err_t matter_controller_reset_acl(uint64_t node_id);
// Clear a device's Binding table (cluster 0x001E on its switch endpoint) so it
// stops sending commands to any unicast or group target. No-op for devices without
// a switch endpoint. Note: this is the *send* side, distinct from reset_acl (*receive*).
esp_err_t matter_controller_reset_bindings(uint64_t node_id);
// Send an OnOff Toggle groupcast to every member of a group on the controller's own
// fabric. Lets the controller drive a group directly — useful to verify a group works
// independently of any bound switch. Toggle mirrors what the bound switch sends.
esp_err_t matter_controller_groupcast_toggle(uint16_t group_id);

esp_err_t matter_factory_reset(void);

#ifdef __cplusplus
}
#endif
