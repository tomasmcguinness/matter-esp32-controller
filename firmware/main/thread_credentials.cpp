#include "thread_credentials.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "mdns.h"
#include "thread_meshcop.h"

static const char *TAG = "thread_creds";

#define LFS_BASE_PATH "/littlefs"
#define THREAD_PATH   LFS_BASE_PATH "/thread.json"
#define THREAD_TMP    LFS_BASE_PATH "/thread.json.tmp"

// Largest plausible Active Operational Dataset is ~254 bytes (single MeshCoP TLV
// payload). Keep a little headroom.
#define MAX_DATASET_LEN 256

// MeshCoP TLV types we surface as metadata.
enum {
    MESHCOP_TLV_CHANNEL          = 0,
    MESHCOP_TLV_PANID            = 1,
    MESHCOP_TLV_EXTENDED_PANID   = 2,
    MESHCOP_TLV_NETWORK_NAME     = 3,
};

struct thread_creds_t {
    bool        has = false;
    std::vector<uint8_t> dataset;     // raw Active Operational Dataset (MeshCoP TLVs)
    std::string networkName;
    int         channel = -1;
    int         panId = -1;           // 16-bit; -1 = unknown
    std::string extPanId;             // hex string
    std::string borderAgentHost;
    int64_t     fetchedAt = 0;        // unix seconds
};

static thread_creds_t s_creds;
static SemaphoreHandle_t s_mutex = nullptr;

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------

static void bytes_to_hex(const uint8_t *data, size_t len, std::string &out)
{
    static const char *digits = "0123456789ABCDEF";
    out.clear();
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(digits[(data[i] >> 4) & 0xF]);
        out.push_back(digits[data[i] & 0xF]);
    }
}

static bool hex_to_bytes(const char *hex, std::vector<uint8_t> &out)
{
    size_t len = strlen(hex);
    if (len % 2 != 0) return false;
    out.clear();
    out.reserve(len / 2);
    for (size_t i = 0; i < len; i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

// ---------------------------------------------------------------------------
// MeshCoP TLV parsing for display metadata
// ---------------------------------------------------------------------------

static void parse_dataset_metadata(const uint8_t *data, size_t len, thread_creds_t &c)
{
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t type = data[i];
        uint8_t tlv_len = data[i + 1];
        size_t value = i + 2;
        if (value + tlv_len > len) break;      // truncated TLV
        const uint8_t *v = data + value;

        switch (type) {
        case MESHCOP_TLV_CHANNEL:
            // ChannelPage(1) + Channel(2, big-endian)
            if (tlv_len >= 3) c.channel = (v[1] << 8) | v[2];
            break;
        case MESHCOP_TLV_PANID:
            if (tlv_len >= 2) c.panId = (v[0] << 8) | v[1];
            break;
        case MESHCOP_TLV_EXTENDED_PANID:
            if (tlv_len == 8) bytes_to_hex(v, 8, c.extPanId);
            break;
        case MESHCOP_TLV_NETWORK_NAME:
            c.networkName.assign((const char *)v, tlv_len);
            break;
        default:
            break;
        }
        i = value + tlv_len;
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static esp_err_t persist_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "hasCredentials", s_creds.has);
    if (s_creds.has) {
        std::string hex;
        bytes_to_hex(s_creds.dataset.data(), s_creds.dataset.size(), hex);
        cJSON_AddStringToObject(root, "dataset", hex.c_str());
        cJSON_AddStringToObject(root, "networkName", s_creds.networkName.c_str());
        cJSON_AddNumberToObject(root, "channel", s_creds.channel);
        cJSON_AddNumberToObject(root, "panId", s_creds.panId);
        cJSON_AddStringToObject(root, "extPanId", s_creds.extPanId.c_str());
        cJSON_AddStringToObject(root, "borderAgentHost", s_creds.borderAgentHost.c_str());
        cJSON_AddNumberToObject(root, "fetchedAt", (double)s_creds.fetchedAt);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;

    esp_err_t result = ESP_OK;
    FILE *f = fopen(THREAD_TMP, "w");
    if (!f) { result = ESP_FAIL; goto done; }
    if (fputs(text, f) == EOF) { fclose(f); result = ESP_FAIL; goto done; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(THREAD_TMP, THREAD_PATH) != 0) result = ESP_FAIL;
done:
    free(text);
    if (result != ESP_OK) ESP_LOGE(TAG, "Failed to persist thread credentials");
    return result;
}

static void load_from_disk(void)
{
    FILE *f = fopen(THREAD_PATH, "r");
    if (!f) { ESP_LOGI(TAG, "No thread credentials file; starting fresh"); return; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4096) { fclose(f); return; }

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *has = cJSON_GetObjectItemCaseSensitive(root, "hasCredentials");
    if (cJSON_IsTrue(has)) {
        cJSON *ds = cJSON_GetObjectItemCaseSensitive(root, "dataset");
        if (cJSON_IsString(ds) && hex_to_bytes(ds->valuestring, s_creds.dataset)) {
            s_creds.has = true;
            parse_dataset_metadata(s_creds.dataset.data(), s_creds.dataset.size(), s_creds);

            cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "borderAgentHost");
            if (cJSON_IsString(host)) s_creds.borderAgentHost = host->valuestring;
            cJSON *at = cJSON_GetObjectItemCaseSensitive(root, "fetchedAt");
            if (cJSON_IsNumber(at)) s_creds.fetchedAt = (int64_t)at->valuedouble;
            ESP_LOGI(TAG, "Loaded Thread credentials for network '%s'", s_creds.networkName.c_str());
        }
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t thread_credentials_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    load_from_disk();
    return ESP_OK;
}

char *thread_credentials_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(root, "hasCredentials", s_creds.has);
    if (s_creds.has) {
        cJSON_AddStringToObject(root, "networkName", s_creds.networkName.c_str());
        if (s_creds.channel >= 0) cJSON_AddNumberToObject(root, "channel", s_creds.channel);
        if (s_creds.panId >= 0)   cJSON_AddNumberToObject(root, "panId", s_creds.panId);
        if (!s_creds.extPanId.empty()) cJSON_AddStringToObject(root, "extPanId", s_creds.extPanId.c_str());
        if (!s_creds.borderAgentHost.empty()) cJSON_AddStringToObject(root, "borderAgentHost", s_creds.borderAgentHost.c_str());
        cJSON_AddNumberToObject(root, "fetchedAt", (double)s_creds.fetchedAt);
    }
    xSemaphoreGive(s_mutex);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

char *thread_credentials_discover_json(uint32_t timeout_ms)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "agents");

    // mDNS is initialised lazily here; harmless if already initialised elsewhere.
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init failed: 0x%x", err);
        return cJSON_PrintUnformatted(root); // empty agents list
    }

    mdns_result_t *results = NULL;
    err = mdns_query_ptr("_meshcop-e", "_udp", timeout_ms, 20, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS query failed: 0x%x", err);
        char *text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return text;
    }

    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "host", r->hostname ? r->hostname : (r->instance_name ? r->instance_name : ""));
        if (r->addr) {
            char ip[46] = {0};
            if (r->addr->addr.type == ESP_IPADDR_TYPE_V4)
                esp_ip4addr_ntoa(&r->addr->addr.u_addr.ip4, ip, sizeof(ip));
            cJSON_AddStringToObject(obj, "ip", ip);
        } else {
            cJSON_AddStringToObject(obj, "ip", "");
        }
        cJSON_AddNumberToObject(obj, "port", r->port);
        // Network name is advertised in the "nn" TXT record.
        for (size_t t = 0; t < r->txt_count; t++) {
            if (r->txt[t].key && strcmp(r->txt[t].key, "nn") == 0 && r->txt[t].value) {
                cJSON_AddStringToObject(obj, "networkName", r->txt[t].value);
                break;
            }
        }
        cJSON_AddItemToArray(arr, obj);
    }
    mdns_query_results_free(results);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

esp_err_t thread_credentials_store(const uint8_t *dataset, size_t dataset_len,
                                   const char *border_agent_host)
{
    if (!dataset || dataset_len == 0 || dataset_len > MAX_DATASET_LEN)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_creds = thread_creds_t{};
    s_creds.has = true;
    s_creds.dataset.assign(dataset, dataset + dataset_len);
    parse_dataset_metadata(dataset, dataset_len, s_creds);
    if (border_agent_host) s_creds.borderAgentHost = border_agent_host;
    s_creds.fetchedAt = (int64_t)time(NULL);
    esp_err_t err = persist_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t thread_credentials_fetch(const char *host, uint16_t port, const char *otpc)
{
    if (!host || !otpc) return ESP_ERR_INVALID_ARG;

    // Run the Thread 1.4 Credential Sharing exchange (DTLS-ECJPAKE + CoAP/MeshCoP)
    // against the border agent and pull the Active Operational Dataset.
    uint8_t dataset[MAX_DATASET_LEN];
    size_t dataset_len = 0;
    esp_err_t err = thread_meshcop_pull_dataset(host, port, otpc,
                                                dataset, sizeof(dataset), &dataset_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Credential retrieval from %s failed: 0x%x", host, err);
        return err;
    }

    // Persist the raw dataset and the parsed display metadata.
    return thread_credentials_store(dataset, dataset_len, host);
}

esp_err_t thread_credentials_get_dataset(uint8_t *buf, size_t buf_size, size_t *out_len)
{
    if (!buf || !out_len) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_creds.has) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_creds.dataset.size() > buf_size) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, s_creds.dataset.data(), s_creds.dataset.size());
    *out_len = s_creds.dataset.size();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool thread_credentials_available(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool has = s_creds.has;
    xSemaphoreGive(s_mutex);
    return has;
}

esp_err_t thread_credentials_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_creds = thread_creds_t{};
    unlink(THREAD_PATH);
    esp_err_t err = persist_locked();
    xSemaphoreGive(s_mutex);
    return err;
}
