#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loads any previously-shared Thread credentials from LittleFS.
// Must be called after the LittleFS partition is mounted (node_manager_init).
esp_err_t thread_credentials_init(void);

// Returns a JSON object describing the stored credentials (metadata only — never
// the raw operational dataset / key material). Caller must free().
//   { "hasCredentials": bool, "networkName": str, "channel": num,
//     "panId": num, "extPanId": str, "fetchedAt": num, "borderAgentHost": str }
char *thread_credentials_get_json(void);

// Discovers Thread 1.4 Border Routers advertising the MeshCoP-ePSKc DNS-SD
// service (_meshcop-e._udp) on the local network. Returns a JSON object:
//   { "agents": [ { "host": str, "ip": str, "port": num, "networkName": str } ] }
// Caller must free().
char *thread_credentials_discover_json(uint32_t timeout_ms);

// Performs the Thread 1.4 Credential Sharing exchange against the given border
// agent: derive ePSKc from the one-time passcode, open a DTLS session, petition
// as a commissioner candidate and pull the Active Operational Dataset, then
// persist it. On success the credentials are available via the getters below.
//
// NOTE: the DTLS-ECJPAKE + CoAP/MeshCoP retrieval engine is not yet implemented
// (see firmware/docs/thread-credential-sharing-feasibility.md). Until it lands
// this returns ESP_ERR_NOT_SUPPORTED. The signature is the stable seam the
// engine plugs into.
esp_err_t thread_credentials_fetch(const char *host, uint16_t port, const char *otpc);

// Parses an Active Operational Dataset (raw MeshCoP TLVs), extracts display
// metadata and persists both the raw dataset and the metadata. Intended to be
// called by the retrieval engine once it has the dataset bytes.
esp_err_t thread_credentials_store(const uint8_t *dataset, size_t dataset_len,
                                   const char *border_agent_host);

// Copies the stored raw Active Operational Dataset into buf. Used by the future
// BLE->Thread commissioning path to provision joining devices. Returns
// ESP_ERR_NOT_FOUND when no credentials are stored.
esp_err_t thread_credentials_get_dataset(uint8_t *buf, size_t buf_size, size_t *out_len);

// True when an operational dataset is currently stored.
bool thread_credentials_available(void);

// Clears the stored credentials (deletes thread.json).
esp_err_t thread_credentials_clear(void);

#ifdef __cplusplus
}
#endif
