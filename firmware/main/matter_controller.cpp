#include "matter_controller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_credentials_issuer.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>

#include "ws_server.h"
#include "cJSON.h"
#include "device_manager.h"
#include "node_manager.h"
#include "pairing_command.h"

#include <app/server/Dnssd.h>
#include <controller/CHIPDeviceController.h>
#include <platform/ESP32/NetworkCommissioningDriver.h>
#include <controller/DevicePairingDelegate.h>
#include <controller/OperationalCredentialsDelegate.h>
#include <credentials/CHIPCert.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CASEAuthTag.h>
#include <lib/core/TLV.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/PASESession.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>
#include <setup_payload/SetupPayload.h>

#include <map>
#include <vector>

using namespace chip;
using namespace chip::app::Clusters;

static const char *TAG = "matter_controller";

static constexpr char kNodeIdCounterKey[] = "MC_NodeIdCnt";
static constexpr char kNodeListKey[]      = "MC_NodeList";
static constexpr size_t kMaxNodes         = 32;
static constexpr uint64_t kFirstDeviceNodeId = 1;

static constexpr uint32_t kDescriptorCluster     = 0x001D;
static constexpr uint32_t kDescriptorDeviceTypeList = 0x0000;
static constexpr uint32_t kDescriptorPartsList   = 0x0003;
static constexpr uint32_t kBasicInfoCluster      = 0x0028;
static constexpr uint32_t kBasicInfoVendorName   = 0x0002;
static constexpr uint32_t kBasicInfoProductName  = 0x0004;
static constexpr uint32_t kBridgedDeviceBasicInfoCluster = 0x0039;
static constexpr uint32_t kBridgedDeviceNodeLabel        = 0x0005;
static constexpr uint32_t kDevTypeRootNode       = 0x0016;

// ---------------------------------------------------------------------------
// Node ID counter (NVS)
// ---------------------------------------------------------------------------

uint64_t matter_controller_allocate_node_id(void)
{
    uint64_t node_id = kFirstDeviceNodeId;
    size_t read_size = sizeof(node_id);
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(kNodeIdCounterKey, &node_id, sizeof(node_id), &read_size);
    uint64_t next = node_id + 1;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(kNodeIdCounterKey, &next, sizeof(next));
    return node_id;
}

// ---------------------------------------------------------------------------
// Node list (NVS blob of uint64_t values)
// ---------------------------------------------------------------------------

static void node_list_add(uint64_t node_id)
{
    uint64_t list[kMaxNodes] = {};
    size_t read_size = sizeof(list);
    size_t count = 0;

    if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
            kNodeListKey, list, sizeof(list), &read_size) == CHIP_NO_ERROR)
    {
        count = read_size / sizeof(uint64_t);
    }

    if (count >= kMaxNodes) {
        ESP_LOGW(TAG, "Node list full, cannot add node 0x%llx", (unsigned long long)node_id);
        return;
    }
    list[count++] = node_id;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(
        kNodeListKey, list, count * sizeof(uint64_t));
}

static void node_list_remove(uint64_t node_id)
{
    uint64_t list[kMaxNodes] = {};
    size_t read_size = sizeof(list);

    if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
            kNodeListKey, list, sizeof(list), &read_size) != CHIP_NO_ERROR)
    {
        return;
    }
    size_t count = read_size / sizeof(uint64_t);
    size_t w = 0;
    for (size_t i = 0; i < count; i++) {
        if (list[i] != node_id)
            list[w++] = list[i];
    }
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(
        kNodeListKey, list, w * sizeof(uint64_t));
}

// ---------------------------------------------------------------------------
// Interrogation: accumulate device types, finalize on done
// ---------------------------------------------------------------------------

// Per-node accumulation: endpoint_id -> list of device type IDs
static std::map<uint64_t, std::map<uint16_t, std::vector<uint32_t>>> s_pending_types;
static SemaphoreHandle_t s_pending_mutex = nullptr;

static uint32_t select_primary_device_type(const std::map<uint16_t, std::vector<uint32_t>> &endpoints)
{
    // Try endpoint 1+ first (non-root endpoints have the real device type)
    for (auto const &[ep_id, types] : endpoints) {
        if (ep_id == 0) continue;
        for (uint32_t dt : types) {
            if (dt != kDevTypeRootNode)
                return dt;
        }
    }
    // Fall back to endpoint 0
    for (auto const &[ep_id, types] : endpoints) {
        for (uint32_t dt : types) {
            if (dt != kDevTypeRootNode)
                return dt;
        }
    }
    return 0;
}

// Signalled by on_interrogation_done so the commissioning call can block until
// devices.json and the canvas node have been written.
static SemaphoreHandle_t s_interrogation_done = nullptr;

// Add the freshly-interrogated device to the ReactFlow canvas (nodes.json).
static void add_canvas_node(uint64_t node_id, uint32_t primary_type)
{
    char id[32];
    snprintf(id, sizeof(id), "%llu", (unsigned long long)node_id);

    // Stagger nodes in a 4-wide grid based on their (1-based) node id.
    uint64_t idx = (node_id > 0) ? (node_id - 1) : 0;
    float x = 80.0f + 200.0f * (float)(idx % 4);
    float y = 80.0f + 160.0f * (float)(idx / 4);

    cJSON *settings = cJSON_CreateObject();
    char label[40];
    snprintf(label, sizeof(label), "Node 0x%llX", (unsigned long long)node_id);
    cJSON_AddStringToObject(settings, "label", label);
    cJSON_AddNumberToObject(settings, "nodeId", (double)node_id);
    cJSON_AddNumberToObject(settings, "deviceType", (double)primary_type);
    char *settings_json = cJSON_PrintUnformatted(settings);
    cJSON_Delete(settings);

    node_manager_upsert(id, x, y, settings_json);
    free(settings_json);
}

static void on_interrogation_attr(uint64_t node_id,
                                  const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data)
{
    if (!data)
        return;

    if (path.mClusterId == kDescriptorCluster)
    {
        if (path.mAttributeId == kDescriptorPartsList)
        {
            chip::TLV::TLVType outer;
            if (data->EnterContainer(outer) != CHIP_NO_ERROR)
                return;
            while (data->Next() == CHIP_NO_ERROR) {
                uint16_t ep_id = 0;
                if (data->Get(ep_id) == CHIP_NO_ERROR) {
                    device_manager_add_endpoint(node_id, ep_id);
                    device_manager_add_endpoint_part(node_id, path.mEndpointId, ep_id);
                }
            }
            data->ExitContainer(outer);
        }
        else if (path.mAttributeId == kDescriptorDeviceTypeList)
        {
            device_manager_add_endpoint(node_id, path.mEndpointId);
            chip::TLV::TLVType list_type;
            if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
                return;
            while (data->Next() == CHIP_NO_ERROR) {
                chip::TLV::TLVType struct_type;
                if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
                    continue;
                uint32_t device_type = 0;
                while (data->Next() == CHIP_NO_ERROR) {
                    if (chip::TLV::TagNumFromTag(data->GetTag()) == 0)
                        data->Get(device_type);
                }
                data->ExitContainer(struct_type);
                if (device_type != 0) {
                    device_manager_add_device_type(node_id, path.mEndpointId, device_type);
                    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
                    s_pending_types[node_id][path.mEndpointId].push_back(device_type);
                    xSemaphoreGive(s_pending_mutex);
                }
            }
            data->ExitContainer(list_type);
        }
    }
    else if (path.mClusterId == kBasicInfoCluster)
    {
        chip::CharSpan str;
        if (data->Get(str) != CHIP_NO_ERROR)
            return;
        if (path.mAttributeId == kBasicInfoVendorName)
            device_manager_set_vendor_name(node_id, str.data(), str.size());
        else if (path.mAttributeId == kBasicInfoProductName)
            device_manager_set_product_name(node_id, str.data(), str.size());
    }
    else if (path.mClusterId == kBridgedDeviceBasicInfoCluster &&
             path.mAttributeId == kBridgedDeviceNodeLabel)
    {
        // Per-endpoint: each bridged child carries its own NodeLabel.
        chip::CharSpan str;
        if (data->Get(str) == CHIP_NO_ERROR) {
            device_manager_add_endpoint(node_id, path.mEndpointId);
            device_manager_set_endpoint_label(node_id, path.mEndpointId, str.data(), str.size());
        }
    }
}

static void on_interrogation_done(uint64_t node_id,
                                  const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &,
                                  const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &)
{
    ESP_LOGI(TAG, "Interrogation complete for node 0x%llx", (unsigned long long)node_id);

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    auto it = s_pending_types.find(node_id);
    uint32_t primary_type = 0;
    if (it != s_pending_types.end()) {
        primary_type = select_primary_device_type(it->second);
        s_pending_types.erase(it);
    }
    xSemaphoreGive(s_pending_mutex);

    device_manager_set_primary_device_type(node_id, primary_type);
    device_manager_log_structure(node_id);
    device_manager_resolve_parents(node_id);
    device_manager_persist();

    // Mirror the device onto the ReactFlow canvas.
    add_canvas_node(node_id, primary_type);

    if (s_interrogation_done)
        xSemaphoreGive(s_interrogation_done);
}

static void interrogate_node(uint64_t node_id)
{
    device_manager_add_device(node_id);

    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> attr_paths;
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> event_paths;
    attr_paths.Alloc(4);
    if (!attr_paths.Get()) {
        ESP_LOGE(TAG, "Failed to allocate attribute paths for interrogation");
        return;
    }

    // Descriptor cluster on all endpoints (wildcard), all attributes.
    attr_paths[0] = chip::app::AttributePathParams(chip::kInvalidEndpointId, kDescriptorCluster, chip::kInvalidAttributeId);
    // BasicInformation VendorName and ProductName from endpoint 0.
    attr_paths[1] = chip::app::AttributePathParams(0, kBasicInfoCluster, kBasicInfoVendorName);
    attr_paths[2] = chip::app::AttributePathParams(0, kBasicInfoCluster, kBasicInfoProductName);
    // BridgedDeviceBasicInformation NodeLabel on all endpoints (wildcard).
    attr_paths[3] = chip::app::AttributePathParams(chip::kInvalidEndpointId, kBridgedDeviceBasicInfoCluster, kBridgedDeviceNodeLabel);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto *cmd = new esp_matter::controller::read_command(
        node_id,
        std::move(attr_paths),
        std::move(event_paths),
        on_interrogation_attr,
        on_interrogation_done,
        nullptr);
    if (cmd)
        cmd->send_command();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

// ---------------------------------------------------------------------------
// Blocking unpair
// ---------------------------------------------------------------------------

struct remove_node_ctx {
    SemaphoreHandle_t done;
    CHIP_ERROR result;
};

static remove_node_ctx s_remove_ctx;

static void remove_node_cb(chip::NodeId remoteNodeId, CHIP_ERROR status)
{
    ESP_LOGI(TAG, "RemoveFabric complete for node 0x%llx: %" CHIP_ERROR_FORMAT,
             (unsigned long long)remoteNodeId, status.Format());
    if (status == CHIP_NO_ERROR)
        node_list_remove(remoteNodeId);
    s_remove_ctx.result = status;
    xSemaphoreGive(s_remove_ctx.done);
}

esp_err_t matter_controller_remove_node(uint64_t node_id)
{
    s_remove_ctx.done = xSemaphoreCreateBinary();
    s_remove_ctx.result = CHIP_NO_ERROR;
    if (!s_remove_ctx.done)
        return ESP_ERR_NO_MEM;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::matter_controller_client::get_instance()
                        .unpair((chip::NodeId)node_id, remove_node_cb);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unpair failed: 0x%x", err);
        vSemaphoreDelete(s_remove_ctx.done);
        return err;
    }

    if (xSemaphoreTake(s_remove_ctx.done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Remove node timed out");
        vSemaphoreDelete(s_remove_ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = (s_remove_ctx.result == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
    vSemaphoreDelete(s_remove_ctx.done);
    return result;
}

// ---------------------------------------------------------------------------
// Blocking on-network commissioning
// ---------------------------------------------------------------------------

struct commission_ctx {
    SemaphoreHandle_t done;
    CHIP_ERROR result;
};

static commission_ctx s_commission_ctx;

static void on_commissioning_success_callback(ScopedNodeId peer_id)
{
    ESP_LOGI(TAG, "Commissioning succeeded for node 0x%llx", (unsigned long long)peer_id.GetNodeId());
    s_commission_ctx.result = CHIP_NO_ERROR;
    xSemaphoreGive(s_commission_ctx.done);
}

static void on_commissioning_failure_callback(ScopedNodeId peer_id,
                                              CHIP_ERROR error,
                                              chip::Controller::CommissioningStage stage,
                                              std::optional<chip::Credentials::AttestationVerificationResult> additional_err_info)
{
    ESP_LOGE(TAG, "Commissioning failed for node 0x%llx: %" CHIP_ERROR_FORMAT,
             (unsigned long long)peer_id.GetNodeId(), error.Format());
    s_commission_ctx.result = error;
    xSemaphoreGive(s_commission_ctx.done);
}

esp_err_t matter_controller_commission_on_network(const char *onboarding_payload, uint64_t *node_id_out)
{
    chip::SetupPayload payload;
    CHIP_ERROR parse_err;

    if (strncmp(onboarding_payload, "MT:", 3) == 0)
        parse_err = chip::QRCodeSetupPayloadParser(onboarding_payload).populatePayload(payload);
    else
        parse_err = chip::ManualSetupPayloadParser(onboarding_payload).populatePayload(payload);

    if (parse_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to parse onboarding payload: %" CHIP_ERROR_FORMAT, parse_err.Format());
        return ESP_ERR_INVALID_ARG;
    }

    s_commission_ctx.done = xSemaphoreCreateBinary();
    s_commission_ctx.result = CHIP_NO_ERROR;
    if (!s_commission_ctx.done)
        return ESP_ERR_NO_MEM;

    chip::NodeId node_id = matter_controller_allocate_node_id();

    esp_matter::controller::pairing_command_callbacks_t callbacks = {
        .commissioning_success_callback = on_commissioning_success_callback,
        .commissioning_failure_callback = on_commissioning_failure_callback,
    };
    esp_matter::controller::pairing_command::get_instance().set_callbacks(callbacks);

    ESP_LOGI(TAG, "Attempting to commission node %llu", node_id);

    // TODO(thread): when commissioning a Thread device (no on-network path
    // available), switch to a BLE->Thread pairing flow and attach the shared
    // Thread operational dataset retrieved via Thread Credential Sharing. The
    // dataset is available from thread_credentials_get_dataset() once the
    // controller has pulled it from a Border Router (see thread_credentials.h).
    // Requires enabling BLE (CONFIG_BT_ENABLED + CHIP BLE) — deferred.

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    //esp_matter::controller::pairing_code(node_id, onboarding_payload);
    esp_matter::controller::pairing_on_network(node_id, payload.setUpPINCode);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (xSemaphoreTake(s_commission_ctx.done, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "Commissioning timed out");
        vSemaphoreDelete(s_commission_ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    CHIP_ERROR result = s_commission_ctx.result;
    vSemaphoreDelete(s_commission_ctx.done);

    if (result == CHIP_NO_ERROR) {
        if (node_id_out)
            *node_id_out = (uint64_t)node_id;
        node_list_add(node_id);
        interrogate_node(node_id);
        return ESP_OK;
    }

    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Blocking BLE + Wi-Fi commissioning
// ---------------------------------------------------------------------------

// Hardcoded Wi-Fi credentials handed to devices over BLE during commissioning.
static constexpr char kCommissioningSsid[]     = "JARVIS";
static constexpr char kCommissioningPassword[] = "pmuvevfu";

esp_err_t matter_controller_commission_ble_wifi(const char *onboarding_payload, uint64_t *node_id_out)
{
    chip::SetupPayload payload;
    CHIP_ERROR parse_err;

    if (strncmp(onboarding_payload, "MT:", 3) == 0)
        parse_err = chip::QRCodeSetupPayloadParser(onboarding_payload).populatePayload(payload);
    else
        parse_err = chip::ManualSetupPayloadParser(onboarding_payload).populatePayload(payload);

    if (parse_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to parse onboarding payload: %" CHIP_ERROR_FORMAT, parse_err.Format());
        return ESP_ERR_INVALID_ARG;
    }

    s_commission_ctx.done = xSemaphoreCreateBinary();
    s_commission_ctx.result = CHIP_NO_ERROR;
    if (!s_commission_ctx.done)
        return ESP_ERR_NO_MEM;

    chip::NodeId node_id = matter_controller_allocate_node_id();

    home_energy_manager::controller::pairing_command_callbacks_t callbacks = {
        .commissioning_success_callback = on_commissioning_success_callback,
        .commissioning_failure_callback = on_commissioning_failure_callback,
    };
    home_energy_manager::controller::pairing_command::get_instance().set_callbacks(callbacks);

    ESP_LOGI(TAG, "Attempting BLE+Wi-Fi commission of node %llu onto SSID '%s'",
             (unsigned long long)node_id, kCommissioningSsid);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t pair_err = home_energy_manager::controller::pairing_command::pairing_code_wifi(
        node_id, kCommissioningSsid, kCommissioningPassword, onboarding_payload);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (pair_err != ESP_OK) {
        ESP_LOGE(TAG, "pairing_code_wifi failed to start: 0x%x", pair_err);
        vSemaphoreDelete(s_commission_ctx.done);
        return pair_err;
    }

    if (xSemaphoreTake(s_commission_ctx.done, pdMS_TO_TICKS(120000)) != pdTRUE) {
        ESP_LOGE(TAG, "Commissioning timed out");
        vSemaphoreDelete(s_commission_ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    CHIP_ERROR result = s_commission_ctx.result;
    vSemaphoreDelete(s_commission_ctx.done);

    if (result == CHIP_NO_ERROR) {
        if (node_id_out)
            *node_id_out = (uint64_t)node_id;
        node_list_add(node_id);
        // Interrogate and wait until devices.json / nodes.json are written so the
        // HTTP response only returns once the device is fully recorded.
        interrogate_node(node_id);
        if (s_interrogation_done &&
            xSemaphoreTake(s_interrogation_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGW(TAG, "Interrogation did not finish in time for node 0x%llx",
                     (unsigned long long)node_id);
        }
        return ESP_OK;
    }

    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Public re-interrogation
// ---------------------------------------------------------------------------

esp_err_t matter_controller_interrogate_node(uint64_t node_id)
{
    interrogate_node(node_id);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete)
        ESP_LOGI(TAG, "kCommissioningComplete");
}

esp_err_t matter_controller_start(void)
{
    s_pending_mutex = xSemaphoreCreateMutex();
    if (!s_pending_mutex)
        return ESP_ERR_NO_MEM;

    s_interrogation_done = xSemaphoreCreateBinary();
    if (!s_interrogation_done)
        return ESP_ERR_NO_MEM;

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::controller_register_commands();
    esp_matter::console::init();
#endif

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: 0x%x", err);
        return err;
    }

#if CONFIG_ENABLE_ETHERNET_TELEMETRY
    // Controller mode disables the Matter server, so the NetworkCommissioning
    // cluster never initialises the Ethernet driver automatically. Call it directly.
    CHIP_ERROR eth_err = chip::DeviceLayer::NetworkCommissioning::ESPEthernetDriver::GetInstance().Init(nullptr);
    if (eth_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ESPEthernetDriver::Init failed: %" CHIP_ERROR_FORMAT, eth_err.Format());
        return ESP_FAIL;
    }
#else
    ESP_LOGI(TAG, "W5500 Ethernet disabled (CONFIG_ENABLE_ETHERNET_TELEMETRY=n)");
#endif

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    err = esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Controller init failed: 0x%x", err);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return err;
    }

    err = esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Commissioner setup failed: 0x%x", err);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return err;
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    ESP_LOGI(TAG, "Matter controller started");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------

esp_err_t matter_factory_reset(void)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    chip::Server::GetInstance().ScheduleFactoryReset();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return ESP_OK;
}
