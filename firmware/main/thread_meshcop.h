#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Performs the Thread 1.4 Credential Sharing (ephemeral-key) MeshCoP exchange
// against the Border Agent advertised at host:port (the _meshcop-e._udp service):
//
//   1. Resolve host (an mDNS ".local" name or an IP literal) to an address.
//   2. Open a DTLS session over UDP using the EC-JPAKE key exchange, with the
//      one-time passcode (otpc) as the J-PAKE password (ephemeral PSKc).
//   3. Petition as a commissioner candidate (TMF c/cp).
//   4. Issue MGMT_ACTIVE_GET (TMF c/ag) and read back the Active Operational
//      Dataset as raw MeshCoP TLVs.
//
// On success the dataset bytes are copied into out (capped at out_size) and the
// length written to *out_len. The caller persists them via
// thread_credentials_store(). Returns an esp_err_t describing the failure stage
// otherwise (the dataset is never partially stored on failure).
esp_err_t thread_meshcop_pull_dataset(const char *host, uint16_t port,
                                      const char *otpc,
                                      uint8_t *out, size_t out_size,
                                      size_t *out_len);

#ifdef __cplusplus
}
#endif
