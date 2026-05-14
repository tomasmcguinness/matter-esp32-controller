#include "web_server.h"

#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <inttypes.h>
#include <dirent.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include "node_manager.h"
#include "matter_controller.h"
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
    config.max_uri_handlers = 16;
    config.max_resp_headers = 20;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    const httpd_uri_t commission_post    = {.uri = "/controller/commission",.method = HTTP_POST,   .handler = controller_commission_post_handler};
    const httpd_uri_t factory_reset      = {.uri = "/api/factory-reset",   .method = HTTP_POST,   .handler = factory_reset_post_handler};
    const httpd_uri_t nodes_get          = {.uri = "/api/nodes",           .method = HTTP_GET,    .handler = nodes_get_handler};
    const httpd_uri_t node_put           = {.uri = "/api/nodes/*",         .method = HTTP_PUT,    .handler = node_put_handler};
    const httpd_uri_t node_delete        = {.uri = "/api/nodes/*",         .method = HTTP_DELETE, .handler = node_delete_handler};
    const httpd_uri_t edge_post          = {.uri = "/api/edges",           .method = HTTP_POST,   .handler = edge_post_handler};
    const httpd_uri_t edge_delete        = {.uri = "/api/edges/*",         .method = HTTP_DELETE, .handler = edge_delete_handler};
    const httpd_uri_t debug_files_list   = {.uri = "/debug/files",         .method = HTTP_GET,    .handler = debug_files_list_handler};
    const httpd_uri_t debug_files_get    = {.uri = "/debug/files/*",       .method = HTTP_GET,    .handler = debug_files_get_handler};
    const httpd_uri_t static_files       = {.uri = "/*",                   .method = HTTP_GET,    .handler = static_get_handler};

    httpd_register_uri_handler(server, &commission_post);
    httpd_register_uri_handler(server, &factory_reset);
    httpd_register_uri_handler(server, &nodes_get);
    httpd_register_uri_handler(server, &node_put);
    httpd_register_uri_handler(server, &node_delete);
    httpd_register_uri_handler(server, &edge_post);
    httpd_register_uri_handler(server, &edge_delete);
    httpd_register_uri_handler(server, &debug_files_list);
    httpd_register_uri_handler(server, &debug_files_get);

    ws_server_init(server);

    httpd_register_uri_handler(server, &static_files);

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}
