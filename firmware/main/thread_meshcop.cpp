#include "thread_meshcop.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"

static const char *TAG = "thread_meshcop";

// MeshCoP TLV types used by the commissioner petition exchange.
enum {
    MESHCOP_TLV_COMMISSIONER_ID      = 10,
    MESHCOP_TLV_COMMISSIONER_SESSION = 11,
    MESHCOP_TLV_STATE                = 16,
};
// State TLV value (int8): Accept = 1, Pending = 0, Reject = -1.
#define MESHCOP_STATE_ACCEPT 1

// Only the MeshCoP key exchange is offered on the DTLS session.
static const int s_ciphersuites[] = { MBEDTLS_TLS_ECJPAKE_WITH_AES_128_CCM_8, 0 };

// ---------------------------------------------------------------------------
// UDP transport (connected socket, used as the mbedTLS BIO)
// ---------------------------------------------------------------------------

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int ret = send(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return -1;  // fatal transport error
    }
    return ret;
}

static int net_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms)
{
    int fd = *(int *)ctx;
    struct timeval tv = { (time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000) };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int ret = recv(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
        return -1;  // fatal transport error
    }
    return ret;
}

// Resolves host (IP literal, mDNS ".local" name, or DNS name) and returns a
// connected UDP socket, or -1.
static int connect_udp(const char *host, uint16_t port)
{
    struct sockaddr_in  a4 = {};
    struct sockaddr_in6 a6 = {};
    int family = 0;

    if (inet_pton(AF_INET, host, &a4.sin_addr) == 1) {
        family = AF_INET;
    } else if (inet_pton(AF_INET6, host, &a6.sin6_addr) == 1) {
        family = AF_INET6;
    } else {
        size_t hl = strlen(host);
        bool is_local = hl > 6 && strcasecmp(host + hl - 6, ".local") == 0;
        if (is_local) {
            char name[64];
            size_t nlen = hl - 6;
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, host, nlen);
            name[nlen] = '\0';
            esp_ip4_addr_t ip4;
            esp_ip6_addr_t ip6;
            if (mdns_query_a(name, 3000, &ip4) == ESP_OK) {
                a4.sin_addr.s_addr = ip4.addr;
                family = AF_INET;
            } else if (mdns_query_aaaa(name, 3000, &ip6) == ESP_OK) {
                memcpy(&a6.sin6_addr, ip6.addr, sizeof(a6.sin6_addr));
                family = AF_INET6;
            }
        }
        if (family == 0) {
            // Last resort: unicast DNS for a regular hostname.
            struct addrinfo hints = {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            char portstr[8];
            snprintf(portstr, sizeof(portstr), "%u", port);
            struct addrinfo *res = NULL;
            if (getaddrinfo(host, portstr, &hints, &res) == 0 && res) {
                int fd = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
                if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
                    close(fd);
                    fd = -1;
                }
                freeaddrinfo(res);
                return fd;
            }
            ESP_LOGE(TAG, "Could not resolve border agent '%s'", host);
            return -1;
        }
    }

    int fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;

    int rc;
    if (family == AF_INET) {
        a4.sin_family = AF_INET;
        a4.sin_port = htons(port);
        rc = connect(fd, (struct sockaddr *)&a4, sizeof(a4));
    } else {
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons(port);
        // Link-local IPv6 needs the egress interface scope.
        if (a6.sin6_addr.s6_addr[0] == 0xFE && (a6.sin6_addr.s6_addr[1] & 0xC0) == 0x80) {
            esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
            if (eth) a6.sin6_scope_id = esp_netif_get_netif_impl_index(eth);
        }
        rc = connect(fd, (struct sockaddr *)&a6, sizeof(a6));
    }
    if (rc != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// DTLS retransmission timer (esp_timer backed; MBEDTLS_TIMING_C not enabled)
// ---------------------------------------------------------------------------

struct dtls_timer_t {
    int64_t  start_us;
    uint32_t int_ms;
    uint32_t fin_ms;
};

static void timer_set_delay(void *data, uint32_t int_ms, uint32_t fin_ms)
{
    dtls_timer_t *t = (dtls_timer_t *)data;
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    t->start_us = esp_timer_get_time();
}

static int timer_get_delay(void *data)
{
    dtls_timer_t *t = (dtls_timer_t *)data;
    if (t->fin_ms == 0) return -1;  // cancelled
    int64_t elapsed_ms = (esp_timer_get_time() - t->start_us) / 1000;
    if (elapsed_ms >= t->fin_ms) return 2;
    if (elapsed_ms >= t->int_ms) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal CoAP client (confirmable POST, piggybacked response) over DTLS
// ---------------------------------------------------------------------------

// Appends one CoAP option. Only short deltas/lengths (< 13) are needed for the
// single-/two-segment TMF Uri-Paths used here.
static bool coap_put_option(uint8_t *buf, size_t cap, size_t *pos,
                            uint8_t delta, const uint8_t *val, size_t len)
{
    if (delta >= 13 || len >= 13) return false;
    if (*pos + 1 + len > cap) return false;
    buf[(*pos)++] = (uint8_t)((delta << 4) | len);
    memcpy(buf + *pos, val, len);
    *pos += len;
    return true;
}

static int coap_build_post(uint8_t *buf, size_t cap, uint16_t msgid,
                           const uint8_t token[4],
                           const char *const *seg, int nseg,
                           const uint8_t *payload, size_t payload_len)
{
    const uint8_t URI_PATH = 11;
    size_t pos = 0;
    if (cap < 8) return -1;
    buf[pos++] = (uint8_t)(0x40 | 0x04);  // Ver=1, Type=CON, TKL=4
    buf[pos++] = 0x02;                    // Code 0.02 POST
    buf[pos++] = (uint8_t)(msgid >> 8);
    buf[pos++] = (uint8_t)(msgid & 0xFF);
    memcpy(buf + pos, token, 4);
    pos += 4;

    uint8_t last_opt = 0;
    for (int i = 0; i < nseg; i++) {
        uint8_t delta = (uint8_t)(URI_PATH - last_opt);
        if (!coap_put_option(buf, cap, &pos, delta, (const uint8_t *)seg[i], strlen(seg[i])))
            return -1;
        last_opt = URI_PATH;
    }
    if (payload_len > 0) {
        if (pos + 1 + payload_len > cap) return -1;
        buf[pos++] = 0xFF;  // payload marker
        memcpy(buf + pos, payload, payload_len);
        pos += payload_len;
    }
    return (int)pos;
}

// Extracts the response code and payload, skipping options.
static bool coap_parse(const uint8_t *buf, size_t len, uint8_t *code,
                       const uint8_t **payload, size_t *payload_len)
{
    if (len < 4) return false;
    uint8_t tkl = buf[0] & 0x0F;
    *code = buf[1];
    *payload = NULL;
    *payload_len = 0;
    size_t pos = (size_t)4 + tkl;
    if (pos > len) return false;

    while (pos < len) {
        if (buf[pos] == 0xFF) {  // payload marker
            pos++;
            *payload = buf + pos;
            *payload_len = len - pos;
            return true;
        }
        uint8_t b = buf[pos++];
        uint16_t delta = b >> 4;
        uint16_t olen = b & 0x0F;
        if (delta == 13) { if (pos >= len) return false; delta = (uint16_t)(buf[pos++] + 13); }
        else if (delta == 14) { if (pos + 2 > len) return false; delta = (uint16_t)(((buf[pos] << 8) | buf[pos + 1]) + 269); pos += 2; }
        else if (delta == 15) return false;
        if (olen == 13) { if (pos >= len) return false; olen = (uint16_t)(buf[pos++] + 13); }
        else if (olen == 14) { if (pos + 2 > len) return false; olen = (uint16_t)(((buf[pos] << 8) | buf[pos + 1]) + 269); pos += 2; }
        else if (olen == 15) return false;
        pos += olen;
        if (pos > len) return false;
    }
    return true;  // valid message, no payload
}

// Sends a confirmable POST and reads back the piggybacked response. resp_buf
// holds the raw response; *resp_payload points into it.
static esp_err_t coap_exchange(mbedtls_ssl_context *ssl,
                               const char *const *seg, int nseg,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *resp_buf, size_t resp_cap, uint8_t *resp_code,
                               const uint8_t **resp_payload, size_t *resp_payload_len)
{
    static uint16_t s_msgid = 0;
    uint16_t msgid = ++s_msgid;
    uint8_t token[4] = { (uint8_t)msgid, (uint8_t)(msgid >> 8), 0x5A, 0xA5 };

    uint8_t req[256];
    int reqlen = coap_build_post(req, sizeof(req), msgid, token, seg, nseg, payload, payload_len);
    if (reqlen < 0) return ESP_ERR_INVALID_SIZE;

    int ret;
    do { ret = mbedtls_ssl_write(ssl, req, reqlen); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret < 0) {
        ESP_LOGE(TAG, "CoAP write failed: -0x%04x", -ret);
        return ESP_FAIL;
    }

    do { ret = mbedtls_ssl_read(ssl, resp_buf, resp_cap); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret <= 0) {
        ESP_LOGE(TAG, "CoAP read failed: -0x%04x", -ret);
        return ESP_FAIL;
    }

    if (!coap_parse(resp_buf, (size_t)ret, resp_code, resp_payload, resp_payload_len))
        return ESP_FAIL;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// MeshCoP TLV lookup (handles the extended-length form)
// ---------------------------------------------------------------------------

static const uint8_t *meshcop_tlv_find(const uint8_t *d, size_t len, uint8_t type, size_t *vlen)
{
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = d[i];
        size_t l = d[i + 1];
        size_t hdr = 2;
        if (l == 0xFF) {  // extended length
            if (i + 4 > len) break;
            l = ((size_t)d[i + 2] << 8) | d[i + 3];
            hdr = 4;
        }
        if (i + hdr + l > len) break;
        if (t == type) {
            *vlen = l;
            return d + i + hdr;
        }
        i += hdr + l;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

esp_err_t thread_meshcop_pull_dataset(const char *host, uint16_t port,
                                      const char *otpc,
                                      uint8_t *out, size_t out_size,
                                      size_t *out_len)
{
    if (!host || !otpc || !out || !out_len || out_size == 0) return ESP_ERR_INVALID_ARG;
    if (port == 0) {
        ESP_LOGE(TAG, "No border agent port provided");
        return ESP_ERR_INVALID_ARG;
    }

    int fd = -1;
    esp_err_t result = ESP_FAIL;
    int ret = 0;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    dtls_timer_t timer = {};
    uint8_t resp[600];
    uint8_t code = 0;
    const uint8_t *pl = NULL;
    size_t pll = 0;
    static const char *pers = "thread_meshcop";

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);

    fd = connect_udp(host, port);
    if (fd < 0) {
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (ret != 0) goto cleanup;

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) goto cleanup;

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);  // EC-JPAKE is password-authenticated
    mbedtls_ssl_conf_ciphersuites(&conf, s_ciphersuites);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_handshake_timeout(&conf, 1000, 16000);
    mbedtls_ssl_conf_read_timeout(&conf, 5000);

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) goto cleanup;

    // The ephemeral PSKc is the ASCII one-time passcode itself.
    ret = mbedtls_ssl_set_hs_ecjpake_password(&ssl, (const unsigned char *)otpc, strlen(otpc));
    if (ret != 0) goto cleanup;

    mbedtls_ssl_set_bio(&ssl, &fd, net_send, NULL, net_recv_timeout);
    mbedtls_ssl_set_timer_cb(&ssl, &timer, timer_set_delay, timer_get_delay);
    mbedtls_ssl_set_mtu(&ssl, 1280);

    do { ret = mbedtls_ssl_handshake(&ssl); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret != 0) {
        char eb[96];
        mbedtls_strerror(ret, eb, sizeof(eb));
        ESP_LOGE(TAG, "DTLS handshake failed: -0x%04x (%s)", -ret, eb);
        // Most commonly a wrong passcode or the BR not being in ephemeral-key mode.
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    ESP_LOGI(TAG, "DTLS(EC-JPAKE) session established with %s:%u", host, port);

    // --- Commissioner petition (TMF c/cp) ---
    {
        static const char *const seg[] = { "c", "cp" };
        const char *cid = "matter-controller";
        size_t cidlen = strlen(cid);
        uint8_t pet[2 + 32];
        pet[0] = MESHCOP_TLV_COMMISSIONER_ID;
        pet[1] = (uint8_t)cidlen;
        memcpy(pet + 2, cid, cidlen);

        result = coap_exchange(&ssl, seg, 2, pet, 2 + cidlen, resp, sizeof(resp), &code, &pl, &pll);
        if (result != ESP_OK) goto cleanup;

        size_t vlen = 0;
        const uint8_t *state = pl ? meshcop_tlv_find(pl, pll, MESHCOP_TLV_STATE, &vlen) : NULL;
        if (!state || vlen < 1 || (int8_t)state[0] != MESHCOP_STATE_ACCEPT) {
            ESP_LOGE(TAG, "Commissioner petition rejected (code 0x%02x, state %d)",
                     code, state ? (int)(int8_t)state[0] : -99);
            result = ESP_ERR_NOT_ALLOWED;
            goto cleanup;
        }
        const uint8_t *sid = meshcop_tlv_find(pl, pll, MESHCOP_TLV_COMMISSIONER_SESSION, &vlen);
        if (sid && vlen == 2)
            ESP_LOGI(TAG, "Commissioner petition accepted (session 0x%04x)", (sid[0] << 8) | sid[1]);
        else
            ESP_LOGI(TAG, "Commissioner petition accepted");
    }

    // --- MGMT_ACTIVE_GET (TMF c/ag): empty Get => full Active Operational Dataset ---
    {
        static const char *const seg[] = { "c", "ag" };
        result = coap_exchange(&ssl, seg, 2, NULL, 0, resp, sizeof(resp), &code, &pl, &pll);
        if (result != ESP_OK) goto cleanup;

        if (!pl || pll == 0) {
            ESP_LOGE(TAG, "MGMT_ACTIVE_GET returned no dataset (code 0x%02x)", code);
            result = ESP_FAIL;
            goto cleanup;
        }
        if (pll > out_size) {
            ESP_LOGE(TAG, "Active dataset too large: %u > %u", (unsigned)pll, (unsigned)out_size);
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        memcpy(out, pl, pll);
        *out_len = pll;
        ESP_LOGI(TAG, "Retrieved Active Operational Dataset (%u bytes)", (unsigned)pll);
        result = ESP_OK;
    }

    mbedtls_ssl_close_notify(&ssl);

cleanup:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (fd >= 0) close(fd);
    return result;
}
