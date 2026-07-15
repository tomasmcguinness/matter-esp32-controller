#include "web_server.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <inttypes.h>
#include <dirent.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "node_manager.h"
#include "device_manager.h"
#include "matter_controller.h"
#include "thread_credentials.h"
#include "ws_server.h"

static const char *TAG = "web_server";

#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_LABEL     "storage"
#define LFS_BASE_PATH    "/littlefs"
#define FILE_READ_CHUNK  1024
#define FS_PATH_MAX      544
#define MAX_POST_BODY    1024

static const char *mime_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)                          return "application/octet-stream";
    if (strcmp(dot, ".html") == 0)     return "text/html";
    if (strcmp(dot, ".css")  == 0)     return "text/css";
    if (strcmp(dot, ".js")   == 0)     return "application/javascript";
    if (strcmp(dot, ".json") == 0)     return "application/json";
    if (strcmp(dot, ".svg")  == 0)     return "image/svg+xml";
    if (strcmp(dot, ".png")  == 0)     return "image/png";
    if (strcmp(dot, ".ico")  == 0)     return "image/x-icon";
    if (strcmp(dot, ".woff2")== 0)     return "font/woff2";
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, mime_type_for(fs_path));
    char buf[FILE_READ_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (status == 201)
        httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static int recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len <= 0 || req->content_len >= buf_size)
        return -1;
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) return -1;
        received += r;
    }
    buf[received] = '\0';
    return received;
}

// ---------------------------------------------------------------------------
// POST /controller/commission
// ---------------------------------------------------------------------------

static esp_err_t controller_commission_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Commissioning request received");

    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(root, "onboardingPayload");
    if (!cJSON_IsString(payload_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing onboardingPayload");
        return ESP_FAIL;
    }
    char payload[256];
    strncpy(payload, payload_item->valuestring, sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Beginning on-network commissioning: %s", payload);

    uint64_t commissioned_node_id = 0;
    esp_err_t err = matter_controller_commission_on_network(payload, &commissioned_node_id);

    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid onboarding payload");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_TIMEOUT) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Commissioning timed out");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Commissioning failed");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "nodeId", (double)commissioned_node_id);
    return send_json(req, resp, 200);
}

// ---------------------------------------------------------------------------
// POST /api/factory-reset
// ---------------------------------------------------------------------------

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t err = matter_factory_reset();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Factory reset failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/devices
// ---------------------------------------------------------------------------

static esp_err_t devices_get_handler(httpd_req_t *req)
{
    char *json = device_manager_get_all_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

// ---------------------------------------------------------------------------
// DELETE /api/devices/:nodeId
// ---------------------------------------------------------------------------

static esp_err_t device_delete_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    const char *node_id_str = last_slash + 1;
    uint64_t node_id = strtoull(node_id_str, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    // Best-effort: remove the fabric from the device, then drop our local records.
    matter_controller_remove_node(node_id);
    device_manager_remove_device(node_id);
    node_manager_delete(node_id_str);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// PUT /api/devices/:nodeId  -> set the device's friendly name
// ---------------------------------------------------------------------------

static esp_err_t device_name_put_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(nm)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    // An empty string clears the custom name (heading reverts to vendor/product).
    esp_err_t err = device_manager_set_device_name(node_id, nm->valuestring, strlen(nm->valuestring));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown device");
        return ESP_FAIL;
    }
    device_manager_persist();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// POST /api/reinterview/:nodeId  -> re-read the device's structure
// ---------------------------------------------------------------------------

static esp_err_t reinterview_post_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_interrogate_node(node_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Re-interview failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// GET /api/onoff/:nodeId  -> { "on": bool }
// ---------------------------------------------------------------------------

static esp_err_t onoff_get_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    bool on = false;
    esp_err_t err = matter_controller_get_onoff(node_id, &on);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read OnOff state");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, on ? "{\"on\":true}" : "{\"on\":false}");
}

// ---------------------------------------------------------------------------
// GET /api/acl/:nodeId  -> { "entries": [ { "privilege", "authMode", "subjects" } ] }
// ---------------------------------------------------------------------------

static esp_err_t acl_get_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    char *json = NULL;
    esp_err_t err = matter_controller_get_acl(node_id, &json);
    if (err != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read ACL");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// ---------------------------------------------------------------------------
// GET /api/bindingtable/:nodeId  -> { "endpoint", "entries": [ { node, endpoint, cluster } ] }
// ---------------------------------------------------------------------------

static esp_err_t bindingtable_get_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    char *json = NULL;
    esp_err_t err = matter_controller_get_binding_table(node_id, &json);
    if (err != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read binding table");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// ---------------------------------------------------------------------------
// GET /api/groupstate/:nodeId  -> { "groupKeyMap":[..], "groupTable":[..] }
// Read-only snapshot of a node's Group Key Management state, for diffing a
// working group member against one that ignores the group multicast.
// ---------------------------------------------------------------------------

static esp_err_t groupstate_get_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    char *json = NULL;
    esp_err_t err = matter_controller_get_group_state(node_id, &json);
    if (err != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read group state");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// ---------------------------------------------------------------------------
// GET /api/threadinfo/:nodeId  -> { "networkName", "extendedPanId", "panId",
// "channel", "routingRole" }. Identifies which Thread network a device is on so
// group members split across two Thread networks can be spotted.
// ---------------------------------------------------------------------------

static esp_err_t threadinfo_get_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    char *json = NULL;
    esp_err_t err = matter_controller_get_thread_info(node_id, &json);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"networkName\":null,\"extendedPanId\":null,\"panId\":null,\"channel\":null,\"routingRole\":null}");
    }
    if (err != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Thread info");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// ---------------------------------------------------------------------------
// PUT /api/onoff/:nodeId  body { "on": bool }
// ---------------------------------------------------------------------------

static esp_err_t onoff_put_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *on_j = cJSON_GetObjectItemCaseSensitive(root, "on");
    if (!cJSON_IsBool(on_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing on");
        return ESP_FAIL;
    }
    bool on = cJSON_IsTrue(on_j);
    cJSON_Delete(root);

    esp_err_t err = matter_controller_set_onoff(node_id, on);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set OnOff state");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// POST /api/identify/:nodeId  (no body) - make the device blink for 15s
// ---------------------------------------------------------------------------

static esp_err_t identify_post_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_identify(node_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to identify device");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// GET /api/nodes
// ---------------------------------------------------------------------------

static esp_err_t nodes_get_handler(httpd_req_t *req)
{
    char *json = node_manager_get_all_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

// ---------------------------------------------------------------------------
// PUT /api/nodes/:nodeId
// ---------------------------------------------------------------------------

static esp_err_t node_put_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    const char *node_id = last_slash + 1;

    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *xj = cJSON_GetObjectItemCaseSensitive(root, "x");
    cJSON *yj = cJSON_GetObjectItemCaseSensitive(root, "y");
    if (!cJSON_IsNumber(xj) || !cJSON_IsNumber(yj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing x/y");
        return ESP_FAIL;
    }
    float x = (float)xj->valuedouble;
    float y = (float)yj->valuedouble;

    char *settings_str = NULL;
    cJSON *settings_j = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(settings_j))
        settings_str = cJSON_PrintUnformatted(settings_j);
    cJSON_Delete(root);

    esp_err_t err = node_manager_upsert(node_id, x, y, settings_str);
    free(settings_str);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// DELETE /api/nodes/:nodeId
// ---------------------------------------------------------------------------

static esp_err_t node_delete_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    const char *node_id = last_slash + 1;

    esp_err_t err = node_manager_delete(node_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

// ---------------------------------------------------------------------------
// POST /api/edges
// ---------------------------------------------------------------------------

static esp_err_t edge_post_handler(httpd_req_t *req)
{
    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *id_j  = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *src_j = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON *tgt_j = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (!cJSON_IsString(id_j) || !cJSON_IsString(src_j) || !cJSON_IsString(tgt_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id/source/target");
        return ESP_FAIL;
    }
    cJSON *sh_j = cJSON_GetObjectItemCaseSensitive(root, "sourceHandle");
    cJSON *th_j = cJSON_GetObjectItemCaseSensitive(root, "targetHandle");

    esp_err_t err = node_manager_upsert_edge(
        id_j->valuestring, src_j->valuestring, tgt_j->valuestring,
        cJSON_IsString(sh_j) ? sh_j->valuestring : NULL,
        cJSON_IsString(th_j) ? th_j->valuestring : NULL);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Persist failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// DELETE /api/edges/:edgeId
// ---------------------------------------------------------------------------

static esp_err_t edge_delete_handler(httpd_req_t *req)
{
    const char *id = req->uri + strlen("/api/edges/");
    if (!*id) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing edge id");
        return ESP_FAIL;
    }
    esp_err_t err = node_manager_delete_edge(id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST/DELETE /api/bindings
//   Unicast (switch->light): { "switchNodeId": N, "lightNodeId": N,
//                              "switchEndpoint"?: N, "lightEndpoint"?: N }
//   Group   (switch->group): { "switchNodeId": N, "groupId": N, "switchEndpoint"?: N }
// Creates/removes a Matter binding so the switch directly controls the target.
// ---------------------------------------------------------------------------

static esp_err_t bindings_request_handler(httpd_req_t *req, bool create)
{
    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *sw_j  = cJSON_GetObjectItemCaseSensitive(root, "switchNodeId");
    if (!cJSON_IsNumber(sw_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing switchNodeId");
        return ESP_FAIL;
    }
    cJSON *swe_j = cJSON_GetObjectItemCaseSensitive(root, "switchEndpoint");
    uint64_t switch_node = (uint64_t)sw_j->valuedouble;
    uint16_t switch_ep   = cJSON_IsNumber(swe_j) ? (uint16_t)swe_j->valuedouble : 0;  // 0 = auto-resolve

    cJSON *grp_j = cJSON_GetObjectItemCaseSensitive(root, "groupId");
    esp_err_t err;
    if (cJSON_IsNumber(grp_j)) {
        uint16_t group_id = (uint16_t)grp_j->valuedouble;
        cJSON_Delete(root);
        err = create
            ? matter_controller_create_group_binding(switch_node, switch_ep, group_id)
            : matter_controller_delete_group_binding(switch_node, switch_ep, group_id);
    } else {
        cJSON *lt_j  = cJSON_GetObjectItemCaseSensitive(root, "lightNodeId");
        if (!cJSON_IsNumber(lt_j)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing lightNodeId/groupId");
            return ESP_FAIL;
        }
        cJSON *lte_j = cJSON_GetObjectItemCaseSensitive(root, "lightEndpoint");
        uint64_t light_node = (uint64_t)lt_j->valuedouble;
        uint16_t light_ep   = cJSON_IsNumber(lte_j) ? (uint16_t)lte_j->valuedouble : 0;
        cJSON_Delete(root);
        err = create
            ? matter_controller_create_binding(switch_node, switch_ep, light_node, light_ep)
            : matter_controller_delete_binding(switch_node, switch_ep, light_node, light_ep);
    }

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            create ? "Failed to create binding" : "Failed to delete binding");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Matter groups
//   POST   /api/groups                       -> allocate ids + create group
//   DELETE /api/groups/:groupId              -> tear down the group locally
//   POST   /api/groups/:groupId/members      body { nodeId, name? } -> add member
//   DELETE /api/groups/:groupId/members/:nodeId               -> remove member
// ---------------------------------------------------------------------------

static esp_err_t groups_create_handler(httpd_req_t *req)
{
    // Body is optional; an object { "name": "..." } customises the group name.
    char body[MAX_POST_BODY + 1];
    int len = recv_body(req, body, sizeof(body));
    char name[32] = "";
    if (len > 0) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
            if (cJSON_IsString(name_j) && name_j->valuestring)
                snprintf(name, sizeof(name), "%s", name_j->valuestring);
            cJSON_Delete(root);
        }
    }

    uint16_t group_id = matter_controller_allocate_group_id();
    esp_err_t err = matter_controller_create_group(group_id, name);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create group");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "groupId", group_id);
    return send_json(req, resp, 200);
}

static esp_err_t group_member_add_handler(httpd_req_t *req)
{
    unsigned int group_id = 0;
    if (sscanf(req->uri, "/api/groups/%u/members", &group_id) != 1 || group_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad group id");
        return ESP_FAIL;
    }

    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *node_j = cJSON_GetObjectItemCaseSensitive(root, "nodeId");
    if (!cJSON_IsNumber(node_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing nodeId");
        return ESP_FAIL;
    }
    cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
    char name[32] = "";
    if (cJSON_IsString(name_j) && name_j->valuestring)
        snprintf(name, sizeof(name), "%s", name_j->valuestring);
    uint64_t node_id = (uint64_t)node_j->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = matter_controller_add_group_member(node_id, (uint16_t)group_id, name);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to add member");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Handles both DELETE /api/groups/:groupId and DELETE /api/groups/:groupId/members/:nodeId.
static esp_err_t groups_delete_handler(httpd_req_t *req)
{
    unsigned int group_id = 0;
    unsigned long long node_id = 0;
    esp_err_t err;
    if (strstr(req->uri, "/members/")) {
        if (sscanf(req->uri, "/api/groups/%u/members/%llu", &group_id, &node_id) != 2 || group_id == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad member path");
            return ESP_FAIL;
        }
        err = matter_controller_remove_group_member((uint64_t)node_id, (uint16_t)group_id);
    } else {
        if (sscanf(req->uri, "/api/groups/%u", &group_id) != 1 || group_id == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad group id");
            return ESP_FAIL;
        }
        err = matter_controller_delete_group((uint16_t)group_id);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Group delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/resetgroups/:nodeId — clear all group state this controller provisioned
// on a device (empty GroupKeyMap + KeySetRemove app keysets + RemoveAllGroups).
static esp_err_t reset_groups_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }
    esp_err_t err = matter_controller_reset_node_groups(node_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Reset groups failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/resetacl/:nodeId — reset a device's ACL to just the controller's
// Administer entry (drops stale Operate/group/corrupt entries).
static esp_err_t reset_acl_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }
    esp_err_t err = matter_controller_reset_acl(node_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Reset ACL failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/resetbindings/:nodeId — clear a device's Binding table (the send side).
static esp_err_t reset_bindings_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    uint64_t node_id = strtoull(last_slash + 1, NULL, 10);
    if (node_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid node id");
        return ESP_FAIL;
    }
    esp_err_t err = matter_controller_reset_bindings(node_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Reset bindings failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/grouptoggle/:groupId — controller-originated OnOff Toggle groupcast to a
// group (diagnostic / manual group control). No body.
static esp_err_t group_toggle_handler(httpd_req_t *req)
{
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
        return ESP_FAIL;
    }
    unsigned int group_id = (unsigned int)strtoul(last_slash + 1, NULL, 10);
    if (group_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid group id");
        return ESP_FAIL;
    }

    esp_err_t err = matter_controller_groupcast_toggle((uint16_t)group_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Groupcast failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t bindings_post_handler(httpd_req_t *req)
{
    return bindings_request_handler(req, true);
}

static esp_err_t bindings_delete_handler(httpd_req_t *req)
{
    return bindings_request_handler(req, false);
}

// ---------------------------------------------------------------------------
// Thread credential sharing
// ---------------------------------------------------------------------------

static esp_err_t thread_credentials_get_handler(httpd_req_t *req)
{
    char *json = thread_credentials_get_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t thread_credentials_delete_handler(httpd_req_t *req)
{
    esp_err_t err = thread_credentials_clear();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Clear failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t thread_borderagents_get_handler(httpd_req_t *req)
{
    char *json = thread_credentials_discover_json(2000);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t thread_credentials_fetch_handler(httpd_req_t *req)
{
    char body[MAX_POST_BODY + 1];
    if (recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *host_j = cJSON_GetObjectItemCaseSensitive(root, "host");
    cJSON *port_j = cJSON_GetObjectItemCaseSensitive(root, "port");
    cJSON *otpc_j = cJSON_GetObjectItemCaseSensitive(root, "otpc");
    if (!cJSON_IsString(host_j) || !cJSON_IsString(otpc_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing host/otpc");
        return ESP_FAIL;
    }
    char host[128];
    char otpc[64];
    strncpy(host, host_j->valuestring, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    strncpy(otpc, otpc_j->valuestring, sizeof(otpc) - 1);
    otpc[sizeof(otpc) - 1] = '\0';
    uint16_t port = cJSON_IsNumber(port_j) ? (uint16_t)port_j->valuedouble : 0;
    cJSON_Delete(root);

    esp_err_t err = thread_credentials_fetch(host, port, otpc);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "error",
            "Credential retrieval engine not yet implemented on this controller.");
        httpd_resp_set_status(req, "501 Not Implemented");
        return send_json(req, resp, 0);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Credential fetch failed");
        return ESP_FAIL;
    }

    // On success, return the freshly-stored credential metadata.
    char *json = thread_credentials_get_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

// ---------------------------------------------------------------------------
// Debug endpoints
// ---------------------------------------------------------------------------

static esp_err_t debug_files_list_handler(httpd_req_t *req)
{
    DIR *dir = opendir(LFS_BASE_PATH);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open LittleFS");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_CreateArray();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        char full[FS_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", LFS_BASE_PATH, entry->d_name);
        struct stat st;
        long size = (stat(full, &st) == 0) ? (long)st.st_size : -1;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", entry->d_name);
        cJSON_AddNumberToObject(obj, "size", size);
        cJSON_AddItemToArray(arr, obj);
    }
    closedir(dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "files", arr);
    return send_json(req, root, 200);
}

static esp_err_t debug_files_get_handler(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/debug/files/");
    if (!*name || strchr(name, '/')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    char fs_path[FS_PATH_MAX];
    snprintf(fs_path, sizeof(fs_path), "%s/%s", LFS_BASE_PATH, name);
    httpd_resp_set_type(req, "text/plain");
    return send_file(req, fs_path);
}

// ---------------------------------------------------------------------------
// Static file handler (SPA fallback to index.html)
// ---------------------------------------------------------------------------

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char fs_path[FS_PATH_MAX];
    if (strcmp(req->uri, "/") == 0)
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
    else
        snprintf(fs_path, sizeof(fs_path), "%s%s", SPIFFS_BASE_PATH, req->uri);

    struct stat st;
    if (stat(fs_path, &st) != 0) {
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
        if (stat(fs_path, &st) != 0) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }
    }
    return send_file(req, fs_path);
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

esp_err_t web_server_start(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path          = SPIFFS_BASE_PATH,
        .partition_label    = SPIFFS_LABEL,
        .max_files          = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: 0x%x", err);
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.lru_purge_enable = true;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 12288;
    // Slot count must cover every httpd_register_uri_handler below PLUS the
    // WebSocket handler registered by ws_server_init(); keep a little headroom.
    config.max_uri_handlers = 36;
    config.max_resp_headers = 20;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    const httpd_uri_t commission_post    = {.uri = "/controller/commission",.method = HTTP_POST,   .handler = controller_commission_post_handler};
    const httpd_uri_t factory_reset      = {.uri = "/api/factory-reset",   .method = HTTP_POST,   .handler = factory_reset_post_handler};
    const httpd_uri_t devices_get        = {.uri = "/api/devices",         .method = HTTP_GET,    .handler = devices_get_handler};
    const httpd_uri_t device_delete      = {.uri = "/api/devices/*",       .method = HTTP_DELETE, .handler = device_delete_handler};
    const httpd_uri_t device_name_put    = {.uri = "/api/devices/*",       .method = HTTP_PUT,    .handler = device_name_put_handler};
    const httpd_uri_t reinterview_post   = {.uri = "/api/reinterview/*",   .method = HTTP_POST,   .handler = reinterview_post_handler};
    const httpd_uri_t onoff_get          = {.uri = "/api/onoff/*",         .method = HTTP_GET,    .handler = onoff_get_handler};
    const httpd_uri_t onoff_put          = {.uri = "/api/onoff/*",         .method = HTTP_PUT,    .handler = onoff_put_handler};
    const httpd_uri_t identify_post      = {.uri = "/api/identify/*",      .method = HTTP_POST,   .handler = identify_post_handler};
    const httpd_uri_t acl_get            = {.uri = "/api/acl/*",           .method = HTTP_GET,    .handler = acl_get_handler};
    const httpd_uri_t bindingtable_get   = {.uri = "/api/bindingtable/*",  .method = HTTP_GET,    .handler = bindingtable_get_handler};
    const httpd_uri_t groupstate_get     = {.uri = "/api/groupstate/*",    .method = HTTP_GET,    .handler = groupstate_get_handler};
    const httpd_uri_t threadinfo_get     = {.uri = "/api/threadinfo/*",    .method = HTTP_GET,    .handler = threadinfo_get_handler};
    const httpd_uri_t nodes_get          = {.uri = "/api/nodes",           .method = HTTP_GET,    .handler = nodes_get_handler};
    const httpd_uri_t node_put           = {.uri = "/api/nodes/*",         .method = HTTP_PUT,    .handler = node_put_handler};
    const httpd_uri_t node_delete        = {.uri = "/api/nodes/*",         .method = HTTP_DELETE, .handler = node_delete_handler};
    const httpd_uri_t edge_post          = {.uri = "/api/edges",           .method = HTTP_POST,   .handler = edge_post_handler};
    const httpd_uri_t edge_delete        = {.uri = "/api/edges/*",         .method = HTTP_DELETE, .handler = edge_delete_handler};
    const httpd_uri_t bindings_post      = {.uri = "/api/bindings",        .method = HTTP_POST,   .handler = bindings_post_handler};
    const httpd_uri_t bindings_delete    = {.uri = "/api/bindings",        .method = HTTP_DELETE, .handler = bindings_delete_handler};
    const httpd_uri_t groups_post        = {.uri = "/api/groups",          .method = HTTP_POST,   .handler = groups_create_handler};
    const httpd_uri_t group_member_post  = {.uri = "/api/groups/*",        .method = HTTP_POST,   .handler = group_member_add_handler};
    const httpd_uri_t groups_delete      = {.uri = "/api/groups/*",        .method = HTTP_DELETE, .handler = groups_delete_handler};
    const httpd_uri_t reset_groups_post  = {.uri = "/api/resetgroups/*",   .method = HTTP_POST,   .handler = reset_groups_handler};
    const httpd_uri_t reset_acl_post     = {.uri = "/api/resetacl/*",      .method = HTTP_POST,   .handler = reset_acl_handler};
    const httpd_uri_t reset_bindings_post = {.uri = "/api/resetbindings/*", .method = HTTP_POST,  .handler = reset_bindings_handler};
    const httpd_uri_t group_toggle_post  = {.uri = "/api/grouptoggle/*",   .method = HTTP_POST,   .handler = group_toggle_handler};
    const httpd_uri_t thread_creds_get   = {.uri = "/api/thread/credentials",      .method = HTTP_GET,    .handler = thread_credentials_get_handler};
    const httpd_uri_t thread_creds_del   = {.uri = "/api/thread/credentials",      .method = HTTP_DELETE, .handler = thread_credentials_delete_handler};
    const httpd_uri_t thread_creds_fetch = {.uri = "/api/thread/credentials/fetch",.method = HTTP_POST,   .handler = thread_credentials_fetch_handler};
    const httpd_uri_t thread_agents_get  = {.uri = "/api/thread/borderagents",     .method = HTTP_GET,    .handler = thread_borderagents_get_handler};
    const httpd_uri_t debug_files_list   = {.uri = "/debug/files",         .method = HTTP_GET,    .handler = debug_files_list_handler};
    const httpd_uri_t debug_files_get    = {.uri = "/debug/files/*",       .method = HTTP_GET,    .handler = debug_files_get_handler};
    const httpd_uri_t static_files       = {.uri = "/*",                   .method = HTTP_GET,    .handler = static_get_handler};

    httpd_register_uri_handler(server, &commission_post);
    httpd_register_uri_handler(server, &factory_reset);
    httpd_register_uri_handler(server, &devices_get);
    httpd_register_uri_handler(server, &device_delete);
    httpd_register_uri_handler(server, &device_name_put);
    httpd_register_uri_handler(server, &reinterview_post);
    httpd_register_uri_handler(server, &onoff_get);
    httpd_register_uri_handler(server, &onoff_put);
    httpd_register_uri_handler(server, &identify_post);
    httpd_register_uri_handler(server, &acl_get);
    httpd_register_uri_handler(server, &bindingtable_get);
    httpd_register_uri_handler(server, &groupstate_get);
    httpd_register_uri_handler(server, &threadinfo_get);
    httpd_register_uri_handler(server, &nodes_get);
    httpd_register_uri_handler(server, &node_put);
    httpd_register_uri_handler(server, &node_delete);
    httpd_register_uri_handler(server, &edge_post);
    httpd_register_uri_handler(server, &edge_delete);
    httpd_register_uri_handler(server, &bindings_post);
    httpd_register_uri_handler(server, &bindings_delete);
    httpd_register_uri_handler(server, &groups_post);
    httpd_register_uri_handler(server, &group_member_post);
    httpd_register_uri_handler(server, &groups_delete);
    httpd_register_uri_handler(server, &reset_groups_post);
    httpd_register_uri_handler(server, &reset_acl_post);
    httpd_register_uri_handler(server, &reset_bindings_post);
    httpd_register_uri_handler(server, &group_toggle_post);
    httpd_register_uri_handler(server, &thread_creds_get);
    httpd_register_uri_handler(server, &thread_creds_del);
    httpd_register_uri_handler(server, &thread_creds_fetch);
    httpd_register_uri_handler(server, &thread_agents_get);
    httpd_register_uri_handler(server, &debug_files_list);
    httpd_register_uri_handler(server, &debug_files_get);

    ws_server_init(server);

    httpd_register_uri_handler(server, &static_files);

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}
