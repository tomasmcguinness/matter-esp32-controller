# Feasibility: Thread 1.4 Credential Sharing engine on ESP32-S3

Spike output for the credential-retrieval **engine** — the part that actually
pulls the Active Operational Dataset from a Border Router. The Thread tab UI,
HTTP API, on-device storage and mDNS discovery are already implemented and
wired behind the stub `thread_credentials_fetch()` in
`main/thread_credentials.cpp`; this document decides how to fill that stub in.

## The mechanism (recap)

Thread 1.4 Credential Sharing is a pure **IP / MeshCoP** flow — no 802.15.4
radio and no Matter fabric membership required (that's why phones can do it):

1. The Border Router is put into ephemeral-key mode; it shows a one-time
   passcode (**OTPC**, a 9-digit numeric code / QR) and advertises the DNS-SD
   service **`_meshcop-e._udp`**. The ephemeral-key DTLS session is independent
   of the Border Agent's permanent PSKc session.
2. The Commissioner Candidate (our controller) discovers the service over mDNS,
   uses the OTPC as the **ePSKc** secret.
3. It opens a **DTLS** session to the Border Agent using **EC-JPAKE** with the
   passcode as the J-PAKE password.
4. Over that secure CoAP/TMF session it petitions as a commissioner and issues
   **`MGMT_ACTIVE_GET`** to retrieve the Active Operational Dataset.

References: Thread 1.4 Features White Paper; Espressif "Thread Network
Credentials Sharing" blog (`developer.espressif.com/blog/2026/01/thread-credential-sharing/`);
OpenThread Border Agent API (ephemeral key); and a working standalone reference
implementation, `phil-margetson/ThreadCommissionerKit` (Swift package: mDNS
discovery + DTLS-ECJPAKE + active-dataset retrieval, no radio) — concrete proof
the client side is implementable off-mesh.

## What was investigated

### 1. mbedTLS DTLS + EC-JPAKE on this ESP-IDF — CONFIRMED AVAILABLE ✅ (decisive)

The make-or-break crypto primitive is present in the project's toolchain
(ESP-IDF v5.5.4, `components/mbedtls`):

- `CONFIG_MBEDTLS_KEY_EXCHANGE_ECJPAKE` (Kconfig:868) — gated on
  `MBEDTLS_ECJPAKE_C` (Kconfig:1077, enabled by default) + SECP256R1. Enables
  the `TLS-ECJPAKE-WITH-AES-128-CCM-8` ciphersuite — exactly what MeshCoP uses.
- `int mbedtls_ssl_set_hs_ecjpake_password(...)` — `mbedtls/ssl.h:4240` — the
  API to feed the passcode as the J-PAKE secret.
- `CONFIG_MBEDTLS_SSL_PROTO_DTLS` (Kconfig:899).

Both are **off by default** in `mbedtls/port/include/mbedtls/esp_config.h` but
enabled via sdkconfig (`CONFIG_MBEDTLS_KEY_EXCHANGE_ECJPAKE=y`,
`CONFIG_MBEDTLS_SSL_PROTO_DTLS=y`). No SDK patching required.

### 2. OpenThread building blocks — bundled in-tree, but radio-coupled ⚠️

ESP-IDF v5.5.4 ships the full OpenThread source at
`components/openthread/openthread/src/core/`, including reusable primitives:

- `meshcop/dataset.{hpp,cpp}`, `meshcop/dataset_manager.*` — Operational Dataset
  TLV encode/decode (authoritative TLV definitions).
- `coap/coap.{hpp,cpp}`, `coap/coap_secure.{hpp,cpp}` — CoAP and **DTLS-secured
  CoAP** (`CoapSecure`/`SecureTransport`) already wired to mbedTLS EC-JPAKE.
- `meshcop/commissioner.{hpp,cpp}` — the commissioner role.

**Caveat:** these classes are the *native on-mesh* commissioner — they assume an
`ot::Instance` and run over OpenThread's own IPv6/UDP stack (i.e. a Thread node
with a radio). The off-mesh "external commissioner over infrastructure" is a
*separate* program (`ot-commissioner`), not the core `MeshCoP::Commissioner`.
Reusing `CoapSecure` standalone means driving it without an `ot::Instance` over
host lwIP sockets — high coupling, fighting the abstraction. The dataset TLV
code, by contrast, is largely self-contained and worth reusing as a reference.

### 3. `ot-commissioner` portability — heaviest path ❌ (not recommended)

`openthread/ot-commissioner` is the canonical external-commissioner library, but:
- POSIX-oriented (Linux/Mac/Android); deps include libevent (`event2`), an mDNS
  lib, JSON and `fmt` — the event loop, sockets and mDNS would all need
  replacing with ESP-IDF equivalents (`esp_mdns`, lwIP sockets).
- Public docs describe Thread **1.1/1.2** commissioning; **ePSKc (1.4) support
  is unconfirmed** in the released library. Porting it *and* adding ePSKc is the
  largest, least-certain option.

### 4. CoAP availability — no standalone component

No `libcoap` managed component and no `components/coap` in this IDF. The only
in-tree CoAP is OpenThread's (radio-coupled, see #2). A commissioner only needs
a *tiny* CoAP client: confirmable POST with a handful of options to two TMF
URIs (commissioner petition, then active-dataset get) — a few hundred lines, not
a general CoAP stack.

## Recommendation

**Hand-roll a minimal MeshCoP external-commissioner client** on the confirmed,
already-present ESP-IDF primitives:

- **DTLS + EC-JPAKE** via mbedTLS over a standard lwIP UDP socket to the border
  agent address from mDNS, with `mbedtls_ssl_set_hs_ecjpake_password()` fed the
  OTPC. (Enable the two sdkconfig options above.)
- **A small CoAP client** (confirmable requests over the DTLS session) issuing
  the TMF commissioner-petition then `MGMT_ACTIVE_GET`.
- **Dataset decode** reusing the MeshCoP TLV layout — we already have a metadata
  parser in `thread_credentials.cpp`; cross-check field encodings against
  OpenThread's `meshcop/dataset.hpp` and store the raw dataset verbatim.

Rationale: every dependency is confirmed present (no SDK patching, no POSIX
porting, no `ot::Instance` coupling); the commissioner's protocol surface is
small and well-documented; and `ThreadCommissionerKit` proves the same client
flow works as a standalone off-mesh program. `ot-commissioner` porting is the
fallback only if hand-rolling the TMF/petition exchange proves more fiddly than
expected.

### Open items to pin down during implementation

- Exact TMF URIs and payloads for the external-commissioner petition and
  `MGMT_ACTIVE_GET` (confirm against OpenThread `tmf`/`uri_paths` + the Thread
  1.4 spec); whether a commissioner petition is required before the active-get
  in ephemeral-key mode, or the BR allows the dataset get directly on the
  ephemeral session.
- Exact bytes used as the EC-JPAKE password (the ASCII digit string of the OTPC
  vs. a derived value) — verify against the BR (`esp-thread-br` ephemeral-key
  example is the reference peer to test against).
- DTLS ciphersuite/curve restriction to `TLS-ECJPAKE-WITH-AES-128-CCM-8` /
  SECP256R1 and any MeshCoP-specific handshake options.

### Integration

The engine implements the body of `thread_credentials_fetch(host, port, otpc)`
in `main/thread_credentials.cpp` and, on success, calls the existing
`thread_credentials_store(dataset, len, host)` — no API, storage, or UI changes
needed. The `POST /api/thread/credentials/fetch` endpoint already returns
`501 Not Implemented` until this lands.

### Test target

`espressif/esp-thread-br` (m5stack_thread_border_router example) supports Thread
1.4 ephemeral-key credential sharing and is the reference Border Router to
validate against end-to-end.
