#include "matter_controller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_credentials_issuer.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_write_command.h>
#include <esp_matter_controller_group_settings.h>

#include <esp_random.h>
#include <mbedtls/base64.h>

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

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace chip;
using namespace chip::app::Clusters;

static const char *TAG = "matter_controller";

static constexpr char kNodeIdCounterKey[] = "MC_NodeIdCnt";
static constexpr char kGroupIdCounterKey[] = "MC_GrpIdCnt";
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
static constexpr uint32_t kOnOffCluster          = 0x0006;
static constexpr uint32_t kOnOffAttribute        = 0x0000;
static constexpr uint32_t kOnOffCmdOff           = 0x00;
static constexpr uint32_t kOnOffCmdOn            = 0x01;
static constexpr uint32_t kOnOffCmdToggle        = 0x02;
static constexpr uint32_t kIdentifyCluster       = 0x0003;
static constexpr uint32_t kIdentifyCmdIdentify   = 0x00;
static constexpr uint16_t kIdentifyTimeSeconds   = 15;

// Binding / Access Control clusters used for switch->light bindings.
static constexpr uint32_t kBindingCluster        = 0x001E;
static constexpr uint32_t kBindingAttribute      = 0x0000;
static constexpr uint32_t kAclCluster            = 0x001F;
static constexpr uint32_t kAclAttribute          = 0x0000;
// Operational node id of this controller (commissioner), see init(112233, 1, 5580) below.
// This is the subject of the Administer ACL entry every commissioned device already holds.
static constexpr uint64_t kControllerNodeId      = 112233ULL;
// AccessControlEntryPrivilegeEnum / AccessControlEntryAuthModeEnum values.
static constexpr uint8_t  kAclPrivilegeOperate   = 3;
static constexpr uint8_t  kAclPrivilegeAdminister = 5;
static constexpr uint8_t  kAclAuthModeCase       = 2;
static constexpr uint8_t  kAclAuthModeGroup      = 3;

// Group Key Management (0x003F) + Groups (0x0004) clusters for Matter groups.
static constexpr uint32_t kGroupKeyMgmtCluster   = 0x003F;
static constexpr uint32_t kGroupKeyMapAttribute  = 0x0000;  // GroupKeyMap (list)
static constexpr uint32_t kGroupTableAttribute   = 0x0001;  // GroupTable / GroupInfoMap (list)
static constexpr uint32_t kKeySetWriteCommand    = 0x0000;
static constexpr uint32_t kKeySetRemoveCommand   = 0x0003;
static constexpr uint32_t kGroupsCluster         = 0x0004;
// Thread Network Diagnostics (0x0035) attributes that identify which Thread network
// a device is on (NetworkName + ExtendedPanId) plus its mesh role.
static constexpr uint32_t kThreadDiagCluster     = 0x0035;
static constexpr uint32_t kThreadChannelAttr     = 0x0000;  // uint16
static constexpr uint32_t kThreadRoutingRoleAttr = 0x0001;  // enum8
static constexpr uint32_t kThreadNetworkNameAttr = 0x0002;  // string
static constexpr uint32_t kThreadPanIdAttr       = 0x0003;  // uint16
static constexpr uint32_t kThreadExtPanIdAttr    = 0x0004;  // uint64
static constexpr uint32_t kAddGroupCommand       = 0x0000;
static constexpr uint32_t kRemoveGroupCommand    = 0x0003;
static constexpr uint32_t kRemoveAllGroupsCommand = 0x0004;
// Upper bound for KeySetRemove sweep when resetting a device's groups. Removing a
// non-existent keyset just returns NOT_FOUND (harmless); covers all app keysets ever
// allocated by the old keyset-per-group scheme.
static constexpr uint16_t kMaxGroupKeysetScan    = 32;
// TrustFirst group key security policy; single 16-byte epoch key per keyset.
static constexpr uint8_t  kGroupKeyPolicyTrustFirst = 0;
static constexpr size_t   kEpochKeyLen           = 16;
// Every group shares this single application keyset (keyset 0 is the IPK). Devices
// have a tiny per-fabric group-key table, so a keyset-per-group exhausts it fast.
static constexpr uint16_t kAppKeysetId           = 1;
// NVS key prefix for persisting each keyset's epoch key (so members added later
// can be issued the same key). Kept out of the web/node layer on purpose.
static constexpr char     kKeysetKeyPrefix[]     = "MC_GK";

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

uint16_t matter_controller_allocate_group_id(void)
{
    // Group ids start at 1 (0 is the reserved "undefined" group id in Matter).
    uint16_t group_id = 1;
    size_t read_size = sizeof(group_id);
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(kGroupIdCounterKey, &group_id, sizeof(group_id), &read_size);
    uint16_t next = group_id + 1;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(kGroupIdCounterKey, &next, sizeof(next));
    return group_id;
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

// Builds the canvas node settings (label / nodeId / deviceType). Caller frees.
static char *build_node_settings_json(uint64_t node_id, uint32_t primary_type)
{
    cJSON *settings = cJSON_CreateObject();
    char label[40];
    snprintf(label, sizeof(label), "Node 0x%llX", (unsigned long long)node_id);
    cJSON_AddStringToObject(settings, "label", label);
    cJSON_AddNumberToObject(settings, "nodeId", (double)node_id);
    cJSON_AddNumberToObject(settings, "deviceType", (double)primary_type);
    char *json = cJSON_PrintUnformatted(settings);
    cJSON_Delete(settings);
    return json;
}

// Create the device's ReactFlow canvas node (nodes.json) at a grid position. Called as soon as
// the device is commissioned, with primary_type 0 (unknown) until interrogation enriches it.
static void add_canvas_node(uint64_t node_id, uint32_t primary_type)
{
    char id[32];
    snprintf(id, sizeof(id), "%llu", (unsigned long long)node_id);

    // Stagger nodes in a 4-wide grid based on their (1-based) node id.
    uint64_t idx = (node_id > 0) ? (node_id - 1) : 0;
    float x = 80.0f + 200.0f * (float)(idx % 4);
    float y = 80.0f + 160.0f * (float)(idx / 4);

    char *settings_json = build_node_settings_json(node_id, primary_type);
    node_manager_upsert(id, x, y, settings_json);
    free(settings_json);
}

// Enrich an already-created canvas node with the discovered device type, preserving its position.
static void update_canvas_node_type(uint64_t node_id, uint32_t primary_type)
{
    char id[32];
    snprintf(id, sizeof(id), "%llu", (unsigned long long)node_id);
    char *settings_json = build_node_settings_json(node_id, primary_type);
    node_manager_update_settings(id, settings_json);
    free(settings_json);
}

static void on_interrogation_attr(uint64_t node_id,
                                  const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data,
                                  const chip::app::StatusIB &status)
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

    // The canvas node was already created at commission time; enrich it with the discovered
    // device type (keeping the position the user may have set).
    update_canvas_node_type(node_id, primary_type);

    if (s_interrogation_done)
        xSemaphoreGive(s_interrogation_done);
}

static void interrogate_node(uint64_t node_id)
{
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
// OnOff read / write
// ---------------------------------------------------------------------------

static SemaphoreHandle_t s_onoff_read_done = nullptr;
static bool s_onoff_read_value = false;
static bool s_onoff_read_ok = false;

static void on_onoff_read_attr(uint64_t node_id,
                               const chip::app::ConcreteDataAttributePath &path,
                               chip::TLV::TLVReader *data,
                               const chip::app::StatusIB &status)
{
    if (!data)
        return;
    if (path.mClusterId == kOnOffCluster && path.mAttributeId == kOnOffAttribute) {
        bool value = false;
        if (data->Get(value) == CHIP_NO_ERROR) {
            s_onoff_read_value = value;
            s_onoff_read_ok = true;
        }
    }
}

static void on_onoff_read_done(uint64_t node_id,
                               const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &,
                               const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &)
{
    if (s_onoff_read_done)
        xSemaphoreGive(s_onoff_read_done);
}

esp_err_t matter_controller_get_onoff(uint64_t node_id, bool *on_out)
{
    if (!on_out)
        return ESP_ERR_INVALID_ARG;

    uint16_t endpoint_id = 0;
    esp_err_t err = device_manager_get_onoff_endpoint(node_id, &endpoint_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No OnOff endpoint for node 0x%llx", (unsigned long long)node_id);
        return err;
    }

    if (!s_onoff_read_done) {
        s_onoff_read_done = xSemaphoreCreateBinary();
        if (!s_onoff_read_done)
            return ESP_ERR_NO_MEM;
    }
    s_onoff_read_ok = false;
    s_onoff_read_value = false;

    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> attr_paths;
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> event_paths;
    attr_paths.Alloc(1);
    if (!attr_paths.Get())
        return ESP_ERR_NO_MEM;
    attr_paths[0] = chip::app::AttributePathParams(endpoint_id, kOnOffCluster, kOnOffAttribute);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto *cmd = new esp_matter::controller::read_command(
        node_id,
        std::move(attr_paths),
        std::move(event_paths),
        on_onoff_read_attr,
        on_onoff_read_done,
        nullptr);
    if (cmd)
        cmd->send_command();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (!cmd)
        return ESP_ERR_NO_MEM;

    if (xSemaphoreTake(s_onoff_read_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "OnOff read timed out for node 0x%llx", (unsigned long long)node_id);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_onoff_read_ok)
        return ESP_FAIL;

    *on_out = s_onoff_read_value;
    return ESP_OK;
}

esp_err_t matter_controller_set_onoff(uint64_t node_id, bool on)
{
    uint16_t endpoint_id = 0;
    esp_err_t err = device_manager_get_onoff_endpoint(node_id, &endpoint_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No OnOff endpoint for node 0x%llx", (unsigned long long)node_id);
        return err;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    err = esp_matter::controller::send_invoke_cluster_command(
        node_id, endpoint_id, kOnOffCluster,
        on ? kOnOffCmdOn : kOnOffCmdOff, "{}");
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK)
        ESP_LOGE(TAG, "OnOff command failed for node 0x%llx: 0x%x", (unsigned long long)node_id, err);
    return err;
}

esp_err_t matter_controller_identify(uint64_t node_id)
{
    // Identify is mandatory on application device-type endpoints. Prefer the
    // device's On/Off endpoint when known; otherwise fall back to endpoint 1,
    // the standard Matter primary application endpoint.
    uint16_t endpoint_id = 1;
    device_manager_get_onoff_endpoint(node_id, &endpoint_id);

    // IdentifyTime (field 0, U16) in seconds; esp-matter parses the data field
    // as {"<tag>:<type>": value}.
    char command_data[32];
    snprintf(command_data, sizeof(command_data), "{\"0:U16\": %u}", (unsigned)kIdentifyTimeSeconds);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::send_invoke_cluster_command(
        node_id, endpoint_id, kIdentifyCluster, kIdentifyCmdIdentify, command_data);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK)
        ESP_LOGE(TAG, "Identify command failed for node 0x%llx: 0x%x", (unsigned long long)node_id, err);
    return err;
}

// ---------------------------------------------------------------------------
// Binding (switch -> light) via Binding + Access Control clusters
// ---------------------------------------------------------------------------

struct binding_target_t {
    uint64_t node;
    uint16_t endpoint;
    uint32_t cluster;
    uint16_t group;     // valid when is_group
    bool     is_group;
};

static SemaphoreHandle_t s_rmw_read_done = nullptr;
static bool s_rmw_read_ok = false;
static std::vector<binding_target_t> s_binding_entries;
static std::vector<uint64_t> s_acl_operate_subjects;

// Full ACL snapshot captured by on_acl_read_full for matter_controller_get_acl
// and for the group-membership ACL read-modify-write.
struct acl_entry_t {
    uint8_t privilege;
    uint8_t auth_mode;
    std::vector<uint64_t> subjects;
};
static std::vector<acl_entry_t> s_acl_entries;

// GroupKeyMap (Group Key Management 0x003F attr 0x0000) entries captured for RMW.
struct group_key_map_t {
    uint16_t group_id;
    uint16_t keyset_id;
};
static std::vector<group_key_map_t> s_group_key_map;

// GroupTable (Group Key Management 0x003F attr 0x0001) entries: which endpoints the
// device believes belong to each group. Captured for read-only diagnostics.
struct group_table_entry_t {
    uint16_t group_id;
    std::vector<uint16_t> endpoints;
    std::string name;
};
static std::vector<group_table_entry_t> s_group_table;

// Thread Network Diagnostics snapshot captured for read-only display. Each field is
// only valid if its has_* flag is set (devices may not implement every attribute).
struct thread_diag_t {
    bool has_channel = false; uint16_t channel = 0;
    bool has_role = false;    uint8_t role = 0;
    bool has_name = false;    std::string name;
    bool has_panid = false;   uint16_t panid = 0;
    bool has_xpanid = false;  uint64_t xpanid = 0;
};
static thread_diag_t s_thread_diag;

static void on_rmw_read_done(uint64_t,
                             const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &,
                             const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &)
{
    if (s_rmw_read_done)
        xSemaphoreGive(s_rmw_read_done);
}

// Captures the existing unicast Binding targets on the switch endpoint.
static void on_binding_read_attr(uint64_t,
                                 const chip::app::ConcreteDataAttributePath &path,
                                 chip::TLV::TLVReader *data,
                                 const chip::app::StatusIB& status)
{
    if (!data || path.mClusterId != kBindingCluster || path.mAttributeId != kBindingAttribute)
        return;
    s_binding_entries.clear();
    chip::TLV::TLVType list_type;
    if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
        return;
    while (data->Next() == CHIP_NO_ERROR) {
        chip::TLV::TLVType struct_type;
        if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
            continue;
        binding_target_t t = {0, 0, 0, 0, false};
        bool has_ep = false, has_cluster = false, has_group = false;
        while (data->Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(data->GetTag());
            if (tag == 1) data->Get(t.node);                                  // Node
            else if (tag == 2) has_group = (data->Get(t.group) == CHIP_NO_ERROR);   // Group
            else if (tag == 3) has_ep = (data->Get(t.endpoint) == CHIP_NO_ERROR);   // Endpoint
            else if (tag == 4) has_cluster = (data->Get(t.cluster) == CHIP_NO_ERROR); // Cluster
        }
        data->ExitContainer(struct_type);
        // Preserve both flavours so a read-modify-write never drops the other:
        // group targets carry a Group id, unicast targets carry Endpoint+Cluster.
        if (has_group) {
            t.is_group = true;
            s_binding_entries.push_back(t);
        } else if (has_ep && has_cluster) {
            s_binding_entries.push_back(t);
        }
    }
    data->ExitContainer(list_type);
    s_rmw_read_ok = true;
}

// Captures the subjects of existing Operate/CASE ACL entries on the light.
static void on_acl_read_attr(uint64_t,
                             const chip::app::ConcreteDataAttributePath &path,
                             chip::TLV::TLVReader *data,
                             const chip::app::StatusIB& status)
{
    if (!data || path.mClusterId != kAclCluster || path.mAttributeId != kAclAttribute)
        return;
    s_acl_operate_subjects.clear();
    chip::TLV::TLVType list_type;
    if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
        return;
    while (data->Next() == CHIP_NO_ERROR) {
        chip::TLV::TLVType struct_type;
        if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
            continue;
        uint8_t privilege = 0, auth_mode = 0;
        std::vector<uint64_t> subjects;
        while (data->Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(data->GetTag());
            if (tag == 1) data->Get(privilege);        // Privilege
            else if (tag == 2) data->Get(auth_mode);   // AuthMode
            else if (tag == 3 && data->GetType() == chip::TLV::kTLVType_Array) { // Subjects
                chip::TLV::TLVType subj_type;
                if (data->EnterContainer(subj_type) == CHIP_NO_ERROR) {
                    while (data->Next() == CHIP_NO_ERROR) {
                        uint64_t subj = 0;
                        if (data->Get(subj) == CHIP_NO_ERROR)
                            subjects.push_back(subj);
                    }
                    data->ExitContainer(subj_type);
                }
            }
        }
        data->ExitContainer(struct_type);
        if (privilege == kAclPrivilegeOperate && auth_mode == kAclAuthModeCase) {
            for (uint64_t s : subjects)
                if (std::find(s_acl_operate_subjects.begin(), s_acl_operate_subjects.end(), s) ==
                    s_acl_operate_subjects.end())
                    s_acl_operate_subjects.push_back(s);
        }
    }
    data->ExitContainer(list_type);
    s_rmw_read_ok = true;
}

// Captures every ACL entry (privilege, auth mode, subjects) into s_acl_entries
// for read-only display via matter_controller_get_acl.
static void on_acl_read_full(uint64_t,
                             const chip::app::ConcreteDataAttributePath &path,
                             chip::TLV::TLVReader *data,
                             const chip::app::StatusIB& status)
{
    if (!data || path.mClusterId != kAclCluster || path.mAttributeId != kAclAttribute)
        return;
    s_acl_entries.clear();
    chip::TLV::TLVType list_type;
    if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
        return;
    while (data->Next() == CHIP_NO_ERROR) {
        chip::TLV::TLVType struct_type;
        if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
            continue;
        acl_entry_t entry = {0, 0, {}};
        while (data->Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(data->GetTag());
            if (tag == 1) data->Get(entry.privilege);        // Privilege
            else if (tag == 2) data->Get(entry.auth_mode);   // AuthMode
            else if (tag == 3 && data->GetType() == chip::TLV::kTLVType_Array) { // Subjects
                chip::TLV::TLVType subj_type;
                if (data->EnterContainer(subj_type) == CHIP_NO_ERROR) {
                    while (data->Next() == CHIP_NO_ERROR) {
                        uint64_t subj = 0;
                        if (data->Get(subj) == CHIP_NO_ERROR)
                            entry.subjects.push_back(subj);
                    }
                    data->ExitContainer(subj_type);
                }
            }
        }
        data->ExitContainer(struct_type);
        s_acl_entries.push_back(std::move(entry));
    }
    data->ExitContainer(list_type);
    s_rmw_read_ok = true;
}

// Captures the existing GroupKeyMap entries on a device (EP0) for read-modify-write.
static void on_groupkeymap_read_attr(uint64_t,
                                     const chip::app::ConcreteDataAttributePath &path,
                                     chip::TLV::TLVReader *data,
                                     const chip::app::StatusIB& status)
{
    if (!data || path.mClusterId != kGroupKeyMgmtCluster || path.mAttributeId != kGroupKeyMapAttribute)
        return;
    s_group_key_map.clear();
    chip::TLV::TLVType list_type;
    if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
        return;
    while (data->Next() == CHIP_NO_ERROR) {
        chip::TLV::TLVType struct_type;
        if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
            continue;
        group_key_map_t e = {0, 0};
        bool has_group = false, has_keyset = false;
        while (data->Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(data->GetTag());
            if (tag == 1) has_group = (data->Get(e.group_id) == CHIP_NO_ERROR);    // GroupId
            else if (tag == 2) has_keyset = (data->Get(e.keyset_id) == CHIP_NO_ERROR); // GroupKeySetID
        }
        data->ExitContainer(struct_type);
        if (has_group && has_keyset)
            s_group_key_map.push_back(e);
    }
    data->ExitContainer(list_type);
    s_rmw_read_ok = true;
}

// Captures the GroupTable (which endpoints are in which group) for read-only display.
static void on_grouptable_read_attr(uint64_t,
                                    const chip::app::ConcreteDataAttributePath &path,
                                    chip::TLV::TLVReader *data,
                                    const chip::app::StatusIB& status)
{
    if (!data || path.mClusterId != kGroupKeyMgmtCluster || path.mAttributeId != kGroupTableAttribute)
        return;
    s_group_table.clear();
    chip::TLV::TLVType list_type;
    if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
        return;
    while (data->Next() == CHIP_NO_ERROR) {
        chip::TLV::TLVType struct_type;
        if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
            continue;
        group_table_entry_t e;
        e.group_id = 0;
        bool has_group = false;
        while (data->Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(data->GetTag());
            if (tag == 1) has_group = (data->Get(e.group_id) == CHIP_NO_ERROR);   // GroupId
            else if (tag == 2 && data->GetType() == chip::TLV::kTLVType_Array) {  // Endpoints
                chip::TLV::TLVType ep_type;
                if (data->EnterContainer(ep_type) == CHIP_NO_ERROR) {
                    while (data->Next() == CHIP_NO_ERROR) {
                        uint16_t ep = 0;
                        if (data->Get(ep) == CHIP_NO_ERROR)
                            e.endpoints.push_back(ep);
                    }
                    data->ExitContainer(ep_type);
                }
            } else if (tag == 3 && data->GetType() == chip::TLV::kTLVType_UTF8String) { // GroupName
                chip::CharSpan str;
                if (data->Get(str) == CHIP_NO_ERROR)
                    e.name.assign(str.data(), str.size());
            }
        }
        data->ExitContainer(struct_type);
        if (has_group)
            s_group_table.push_back(std::move(e));
    }
    data->ExitContainer(list_type);
    s_rmw_read_ok = true;
}

// Captures individual Thread Network Diagnostics attributes into s_thread_diag.
// Called once per attribute across several targeted reads; the struct is reset by
// the caller before the first read so partial results still populate.
static void on_thread_diag_read_attr(uint64_t,
                                     const chip::app::ConcreteDataAttributePath &path,
                                     chip::TLV::TLVReader *data,
                                     const chip::app::StatusIB&)
{
    if (!data || path.mClusterId != kThreadDiagCluster)
        return;
    switch (path.mAttributeId) {
    case kThreadChannelAttr:
        if (data->Get(s_thread_diag.channel) == CHIP_NO_ERROR) s_thread_diag.has_channel = true;
        break;
    case kThreadRoutingRoleAttr:
        if (data->Get(s_thread_diag.role) == CHIP_NO_ERROR) s_thread_diag.has_role = true;
        break;
    case kThreadNetworkNameAttr: {
        chip::CharSpan str;
        if (data->Get(str) == CHIP_NO_ERROR) {
            s_thread_diag.name.assign(str.data(), str.size());
            s_thread_diag.has_name = true;
        }
        break;
    }
    case kThreadPanIdAttr:
        if (data->Get(s_thread_diag.panid) == CHIP_NO_ERROR) s_thread_diag.has_panid = true;
        break;
    case kThreadExtPanIdAttr:
        if (data->Get(s_thread_diag.xpanid) == CHIP_NO_ERROR) s_thread_diag.has_xpanid = true;
        break;
    }
    s_rmw_read_ok = true;
}

static esp_err_t blocking_read_attr(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                                    uint32_t attribute_id,
                                    esp_matter::controller::attribute_report_cb_t attr_cb)
{
    if (!s_rmw_read_done) {
        s_rmw_read_done = xSemaphoreCreateBinary();
        if (!s_rmw_read_done)
            return ESP_ERR_NO_MEM;
    }
    s_rmw_read_ok = false;
    // Drain any stale signal left by a previous read that timed out (returned
    // ESP_ERR_TIMEOUT) but completed afterwards, so we block on THIS read's
    // completion instead of returning immediately on the leftover signal.
    xSemaphoreTake(s_rmw_read_done, 0);

    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> attr_paths;
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> event_paths;
    attr_paths.Alloc(1);
    if (!attr_paths.Get())
        return ESP_ERR_NO_MEM;
    attr_paths[0] = chip::app::AttributePathParams(endpoint_id, cluster_id, attribute_id);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto *cmd = new esp_matter::controller::read_command(
        node_id, 
        std::move(attr_paths), 
        std::move(event_paths),
        attr_cb, 
        on_rmw_read_done, 
        nullptr,
        nullptr,
        nullptr,
        true);
    if (cmd)
        cmd->send_command();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (!cmd)
        return ESP_ERR_NO_MEM;

    if (xSemaphoreTake(s_rmw_read_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Attribute read timed out for node 0x%llx", (unsigned long long)node_id);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_rmw_read_ok)
        return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t blocking_write_attr(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, const char *json_value)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::send_write_attr_command(
        node_id, endpoint_id, cluster_id, attribute_id, json_value, chip::NullOptional);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Write attr failed node 0x%llx cluster 0x%lx: 0x%x",
                 (unsigned long long)node_id, (unsigned long)cluster_id, err);
    return err;
}

// Builds the json_to_tlv value for the light's ACL: a fixed controller Administer entry plus
// (when any switches are bound) a single Operate entry listing all switch subjects. The
// Administer entry is reconstructed (not copied) so the controller can never lock itself out.
static std::string build_acl_json(uint64_t controller_node_id, const std::vector<uint64_t> &operate_subjects)
{
    char buf[96];
    std::string json = "{\"0:ARR-OBJ\":[";
    snprintf(buf, sizeof(buf),
             "{\"1:U8\":%u,\"2:U8\":%u,\"3:ARR-U64\":[\"%llu\"],\"4:NULL\":null}",
             (unsigned)kAclPrivilegeAdminister, (unsigned)kAclAuthModeCase,
             (unsigned long long)controller_node_id);
    json += buf;
    if (!operate_subjects.empty()) {
        snprintf(buf, sizeof(buf), ",{\"1:U8\":%u,\"2:U8\":%u,\"3:ARR-U64\":[",
                 (unsigned)kAclPrivilegeOperate, (unsigned)kAclAuthModeCase);
        json += buf;
        for (size_t i = 0; i < operate_subjects.size(); ++i) {
            snprintf(buf, sizeof(buf), "%s\"%llu\"", i ? "," : "",
                     (unsigned long long)operate_subjects[i]);
            json += buf;
        }
        json += "],\"4:NULL\":null}";
    }
    json += "]}";
    return json;
}

static std::string build_binding_json(const std::vector<binding_target_t> &targets)
{
    char buf[128];
    std::string json = "{\"0:ARR-OBJ\":[";
    for (size_t i = 0; i < targets.size(); ++i) {
        const binding_target_t &t = targets[i];
        if (t.is_group)
            // Group target: Group (tag 2) + Cluster (tag 4), no Node/Endpoint.
            snprintf(buf, sizeof(buf), "%s{\"2:U16\":%u,\"4:U32\":%lu}",
                     i ? "," : "", (unsigned)t.group, (unsigned long)t.cluster);
        else
            snprintf(buf, sizeof(buf), "%s{\"1:U64\":\"%llu\",\"3:U16\":%u,\"4:U32\":%lu}",
                     i ? "," : "", (unsigned long long)t.node,
                     (unsigned)t.endpoint, (unsigned long)t.cluster);
        json += buf;
    }
    json += "]}";
    return json;
}

esp_err_t matter_controller_get_acl(uint64_t node_id, char **json_out)
{
    if (!json_out)
        return ESP_ERR_INVALID_ARG;
    *json_out = nullptr;

    s_acl_entries.clear();
    esp_err_t err = blocking_read_attr(node_id, 0, kAclCluster, kAclAttribute, on_acl_read_full);
    if (err != ESP_OK)
        return err;

    // Subjects are emitted as JSON strings to avoid 64-bit precision loss in JS.
    std::string json = "{\"entries\":[";
    for (size_t i = 0; i < s_acl_entries.size(); ++i) {
        const acl_entry_t &e = s_acl_entries[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s{\"privilege\":%u,\"authMode\":%u,\"subjects\":[",
                 i ? "," : "", (unsigned)e.privilege, (unsigned)e.auth_mode);
        json += buf;
        for (size_t j = 0; j < e.subjects.size(); ++j) {
            snprintf(buf, sizeof(buf), "%s\"%llu\"", j ? "," : "",
                     (unsigned long long)e.subjects[j]);
            json += buf;
        }
        json += "]}";
    }
    json += "]}";

    char *out = (char *)malloc(json.size() + 1);
    if (!out)
        return ESP_ERR_NO_MEM;
    memcpy(out, json.c_str(), json.size() + 1);
    *json_out = out;
    return ESP_OK;
}

esp_err_t matter_controller_get_binding_table(uint64_t node_id, char **json_out)
{
    if (!json_out)
        return ESP_ERR_INVALID_ARG;
    *json_out = nullptr;

    s_binding_entries.clear();

    // The Binding cluster lives on the device's switch endpoint. If the device has
    // no switch endpoint it cannot hold bindings; report an empty table rather than
    // an error so the UI shows a clean "no bindings" state.
    uint16_t endpoint = 0;
    esp_err_t err = device_manager_get_switch_endpoint(node_id, &endpoint);
    if (err == ESP_OK) {
        err = blocking_read_attr(node_id, endpoint, kBindingCluster, kBindingAttribute,
                                 on_binding_read_attr);
        if (err != ESP_OK)
            return err;
    }

    std::string json = "{\"endpoint\":";
    if (err == ESP_OK)
        json += std::to_string((unsigned)endpoint);
    else
        json += "null";
    json += ",\"entries\":[";
    // node is a JSON string to avoid 64-bit precision loss in JS. Group bindings
    // carry a group id (no node/endpoint) and are emitted with a "group" field.
    for (size_t i = 0; i < s_binding_entries.size(); ++i) {
        const binding_target_t &t = s_binding_entries[i];
        char buf[96];
        if (t.is_group)
            snprintf(buf, sizeof(buf), "%s{\"group\":%u,\"cluster\":%lu}",
                     i ? "," : "", (unsigned)t.group, (unsigned long)t.cluster);
        else
            snprintf(buf, sizeof(buf), "%s{\"node\":\"%llu\",\"endpoint\":%u,\"cluster\":%lu}",
                     i ? "," : "", (unsigned long long)t.node,
                     (unsigned)t.endpoint, (unsigned long)t.cluster);
        json += buf;
    }
    json += "]}";

    char *out = (char *)malloc(json.size() + 1);
    if (!out)
        return ESP_ERR_NO_MEM;
    memcpy(out, json.c_str(), json.size() + 1);
    *json_out = out;
    return ESP_OK;
}

// Read-only snapshot of a node's group state (EP0 Group Key Management cluster):
// the GroupKeyMap (group -> keyset bindings) and the GroupTable (group -> member
// endpoints). Intended for diffing a working member against a non-responding one.
// { "groupKeyMap":[{"group":N,"keyset":N}],
//   "groupTable":[{"group":N,"endpoints":[N,...],"name":"..."}] }
esp_err_t matter_controller_get_group_state(uint64_t node_id, char **json_out)
{
    if (!json_out)
        return ESP_ERR_INVALID_ARG;
    *json_out = nullptr;

    s_group_key_map.clear();
    esp_err_t err = blocking_read_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupKeyMapAttribute,
                                       on_groupkeymap_read_attr);
    if (err != ESP_OK)
        return err;

    s_group_table.clear();
    err = blocking_read_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupTableAttribute,
                             on_grouptable_read_attr);
    if (err != ESP_OK)
        return err;

    std::string json = "{\"groupKeyMap\":[";
    for (size_t i = 0; i < s_group_key_map.size(); ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s{\"group\":%u,\"keyset\":%u}",
                 i ? "," : "", (unsigned)s_group_key_map[i].group_id,
                 (unsigned)s_group_key_map[i].keyset_id);
        json += buf;
    }
    json += "],\"groupTable\":[";
    for (size_t i = 0; i < s_group_table.size(); ++i) {
        const group_table_entry_t &e = s_group_table[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s{\"group\":%u,\"endpoints\":[",
                 i ? "," : "", (unsigned)e.group_id);
        json += buf;
        for (size_t j = 0; j < e.endpoints.size(); ++j) {
            snprintf(buf, sizeof(buf), "%s%u", j ? "," : "", (unsigned)e.endpoints[j]);
            json += buf;
        }
        json += "],\"name\":\"";
        // Escape the group name defensively; it is device-supplied.
        for (char c : e.name) {
            if (c == '"' || c == '\\') json += '\\';
            if ((unsigned char)c >= 0x20) json += c;
        }
        json += "\"}";
    }
    json += "]}";

    char *out = (char *)malloc(json.size() + 1);
    if (!out)
        return ESP_ERR_NO_MEM;
    memcpy(out, json.c_str(), json.size() + 1);
    *json_out = out;
    return ESP_OK;
}

// Read-only snapshot of a device's Thread Network Diagnostics (cluster 0x0035, EP0):
// which Thread network it is on (NetworkName + ExtendedPanId + PanId + Channel) and
// its mesh RoutingRole. Intended for spotting members that sit on a *different* Thread
// network (so the controller's group multicast never reaches them). Fields absent on
// the device are emitted as null. Reads are best-effort per attribute.
esp_err_t matter_controller_get_thread_info(uint64_t node_id, char **json_out)
{
    if (!json_out)
        return ESP_ERR_INVALID_ARG;
    *json_out = nullptr;

    s_thread_diag = thread_diag_t{};
    blocking_read_attr(node_id, 0, kThreadDiagCluster, kThreadNetworkNameAttr, on_thread_diag_read_attr);
    blocking_read_attr(node_id, 0, kThreadDiagCluster, kThreadExtPanIdAttr,    on_thread_diag_read_attr);
    blocking_read_attr(node_id, 0, kThreadDiagCluster, kThreadPanIdAttr,       on_thread_diag_read_attr);
    blocking_read_attr(node_id, 0, kThreadDiagCluster, kThreadChannelAttr,     on_thread_diag_read_attr);
    blocking_read_attr(node_id, 0, kThreadDiagCluster, kThreadRoutingRoleAttr, on_thread_diag_read_attr);

    const thread_diag_t &d = s_thread_diag;
    // A device on no Thread network (e.g. reachable only over Wi-Fi/Ethernet) returns
    // nothing for every attribute; report that rather than a misleading empty network.
    if (!d.has_name && !d.has_xpanid && !d.has_panid && !d.has_channel && !d.has_role)
        return ESP_ERR_NOT_FOUND;

    std::string json = "{";
    char buf[96];
    if (d.has_name) {
        json += "\"networkName\":\"";
        for (char c : d.name) {
            if (c == '"' || c == '\\') json += '\\';
            if ((unsigned char)c >= 0x20) json += c;
        }
        json += "\",";
    } else {
        json += "\"networkName\":null,";
    }
    if (d.has_xpanid) {
        snprintf(buf, sizeof(buf), "\"extendedPanId\":\"0x%016llX\",", (unsigned long long)d.xpanid);
        json += buf;
    } else {
        json += "\"extendedPanId\":null,";
    }
    if (d.has_panid)   snprintf(buf, sizeof(buf), "\"panId\":%u,", (unsigned)d.panid);
    else               snprintf(buf, sizeof(buf), "\"panId\":null,");
    json += buf;
    if (d.has_channel) snprintf(buf, sizeof(buf), "\"channel\":%u,", (unsigned)d.channel);
    else               snprintf(buf, sizeof(buf), "\"channel\":null,");
    json += buf;
    if (d.has_role)    snprintf(buf, sizeof(buf), "\"routingRole\":%u}", (unsigned)d.role);
    else               snprintf(buf, sizeof(buf), "\"routingRole\":null}");
    json += buf;

    char *out = (char *)malloc(json.size() + 1);
    if (!out)
        return ESP_ERR_NO_MEM;
    memcpy(out, json.c_str(), json.size() + 1);
    *json_out = out;
    return ESP_OK;
}

esp_err_t matter_controller_create_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                           uint64_t light_node_id, uint16_t light_endpoint)
{
    ESP_LOGI(TAG, "create_binding: requested switch 0x%llx ep%u -> light 0x%llx ep%u (ep 0 = auto-resolve)",
             (unsigned long long)switch_node_id, switch_endpoint,
             (unsigned long long)light_node_id, light_endpoint);

    esp_err_t err;
    if (switch_endpoint == 0) {
        err = device_manager_get_switch_endpoint(switch_node_id, &switch_endpoint);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No switch endpoint for node 0x%llx", (unsigned long long)switch_node_id);
            return err;
        }
        ESP_LOGI(TAG, "create_binding: resolved switch endpoint to ep%u for node 0x%llx",
                 switch_endpoint, (unsigned long long)switch_node_id);
    }
    if (light_endpoint == 0) {
        err = device_manager_get_onoff_endpoint(light_node_id, &light_endpoint);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No OnOff endpoint for node 0x%llx", (unsigned long long)light_node_id);
            return err;
        }
        ESP_LOGI(TAG, "create_binding: resolved light endpoint to ep%u for node 0x%llx",
                 light_endpoint, (unsigned long long)light_node_id);
    }

    // 1. Grant the switch Operate access on the light's ACL (read-modify-write, preserving the
    //    controller's Administer entry). Done before the binding so the switch's first command
    //    is already authorized.
    ESP_LOGI(TAG, "create_binding: reading ACL on light 0x%llx ep0", (unsigned long long)light_node_id);
    err = blocking_read_attr(light_node_id, 0, kAclCluster, kAclAttribute, on_acl_read_attr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create_binding: ACL read on light 0x%llx failed: 0x%x",
                 (unsigned long long)light_node_id, err);
        return err;
    }
    std::vector<uint64_t> subjects = s_acl_operate_subjects;
    ESP_LOGI(TAG, "create_binding: light 0x%llx has %u existing Operate subject(s)",
             (unsigned long long)light_node_id, (unsigned)subjects.size());
    if (std::find(subjects.begin(), subjects.end(), switch_node_id) == subjects.end()) {
        subjects.push_back(switch_node_id);
        ESP_LOGI(TAG, "create_binding: adding switch 0x%llx as Operate subject (now %u subject(s))",
                 (unsigned long long)switch_node_id, (unsigned)subjects.size());
    } else {
        ESP_LOGI(TAG, "create_binding: switch 0x%llx already an Operate subject, ACL unchanged",
                 (unsigned long long)switch_node_id);
    }
    std::string acl_json = build_acl_json(kControllerNodeId, subjects);
    ESP_LOGI(TAG, "create_binding: writing ACL to light 0x%llx: %s",
             (unsigned long long)light_node_id, acl_json.c_str());
    err = blocking_write_attr(light_node_id, 0, kAclCluster, kAclAttribute, acl_json.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create_binding: ACL write to light 0x%llx failed: 0x%x",
                 (unsigned long long)light_node_id, err);
        return err;
    }

    // 2. Add the light as a binding target on the switch (read-modify-write so existing bindings
    //    on the switch endpoint are preserved).
    ESP_LOGI(TAG, "create_binding: reading Binding list on switch 0x%llx ep%u",
             (unsigned long long)switch_node_id, switch_endpoint);
    err = blocking_read_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                             on_binding_read_attr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create_binding: Binding read on switch 0x%llx ep%u failed: 0x%x",
                 (unsigned long long)switch_node_id, switch_endpoint, err);
        return err;
    }
    std::vector<binding_target_t> targets = s_binding_entries;
    ESP_LOGI(TAG, "create_binding: switch 0x%llx ep%u has %u existing binding target(s)",
             (unsigned long long)switch_node_id, switch_endpoint, (unsigned)targets.size());
    bool exists = false;
    for (const auto &t : targets)
        if (t.node == light_node_id && t.endpoint == light_endpoint && t.cluster == kOnOffCluster)
            { exists = true; break; }
    if (!exists) {
        targets.push_back({light_node_id, light_endpoint, kOnOffCluster});
        ESP_LOGI(TAG, "create_binding: adding light 0x%llx ep%u (OnOff) target (now %u target(s))",
                 (unsigned long long)light_node_id, light_endpoint, (unsigned)targets.size());
    } else {
        ESP_LOGI(TAG, "create_binding: light 0x%llx ep%u already a binding target, list unchanged",
                 (unsigned long long)light_node_id, light_endpoint);
    }
    std::string binding_json = build_binding_json(targets);
    ESP_LOGI(TAG, "create_binding: writing Binding list to switch 0x%llx ep%u: %s",
             (unsigned long long)switch_node_id, switch_endpoint, binding_json.c_str());
    err = blocking_write_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                              binding_json.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create_binding: Binding write to switch 0x%llx ep%u failed: 0x%x",
                 (unsigned long long)switch_node_id, switch_endpoint, err);
        return err;
    }

    ESP_LOGI(TAG, "Created binding switch 0x%llx ep%u -> light 0x%llx ep%u",
             (unsigned long long)switch_node_id, switch_endpoint,
             (unsigned long long)light_node_id, light_endpoint);
    return ESP_OK;
}

esp_err_t matter_controller_delete_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                           uint64_t light_node_id, uint16_t light_endpoint)
{
    if (switch_endpoint == 0 &&
        device_manager_get_switch_endpoint(switch_node_id, &switch_endpoint) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    if (light_endpoint == 0)
        device_manager_get_onoff_endpoint(light_node_id, &light_endpoint);  // best effort

    // 1. Remove the light target from the switch's Binding list.
    if (blocking_read_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                           on_binding_read_attr) == ESP_OK) {
        std::vector<binding_target_t> targets;
        for (const auto &t : s_binding_entries) {
            bool match = t.node == light_node_id && t.cluster == kOnOffCluster &&
                         (light_endpoint == 0 || t.endpoint == light_endpoint);
            if (!match)
                targets.push_back(t);
        }
        std::string binding_json = build_binding_json(targets);
        blocking_write_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                            binding_json.c_str());
    }

    // 2. Remove the switch subject from the light's ACL Operate entry (always keep the controller
    //    Administer entry).
    if (blocking_read_attr(light_node_id, 0, kAclCluster, kAclAttribute, on_acl_read_attr) == ESP_OK) {
        std::vector<uint64_t> subjects;
        for (uint64_t s : s_acl_operate_subjects)
            if (s != switch_node_id)
                subjects.push_back(s);
        std::string acl_json = build_acl_json(kControllerNodeId, subjects);
        blocking_write_attr(light_node_id, 0, kAclCluster, kAclAttribute, acl_json.c_str());
    }

    ESP_LOGI(TAG, "Deleted binding switch 0x%llx -> light 0x%llx",
             (unsigned long long)switch_node_id, (unsigned long long)light_node_id);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Matter groups (Group Key Management 0x003F + Groups 0x0004)
// ---------------------------------------------------------------------------

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out /*>= 2*len+1*/)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

// Persist/look up a keyset's 16-byte epoch key in NVS so members added after the
// group is created can be issued the identical key (KeySetWrite).
static esp_err_t keyset_key_store(uint16_t keyset_id, const uint8_t key[kEpochKeyLen])
{
    char k[16];
    snprintf(k, sizeof(k), "%s%u", kKeysetKeyPrefix, (unsigned)keyset_id);
    return chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(k, key, kEpochKeyLen) == CHIP_NO_ERROR
               ? ESP_OK : ESP_FAIL;
}

static bool keyset_key_load(uint16_t keyset_id, uint8_t key[kEpochKeyLen])
{
    char k[16];
    snprintf(k, sizeof(k), "%s%u", kKeysetKeyPrefix, (unsigned)keyset_id);
    size_t read_size = kEpochKeyLen;
    return chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(k, key, kEpochKeyLen, &read_size) ==
               CHIP_NO_ERROR && read_size == kEpochKeyLen;
}

static esp_err_t blocking_invoke_cmd(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t command_id, const char *command_data)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::send_invoke_cluster_command(node_id, endpoint_id, cluster_id, command_id, command_data);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Invoke node 0x%llx cluster 0x%lx cmd 0x%lx failed: 0x%x", (unsigned long long)node_id, (unsigned long)cluster_id, (unsigned long)command_id, err);
    }
    return err;
}

static std::string build_groupkeymap_json(const std::vector<group_key_map_t> &entries)
{
    char buf[64];
    std::string json = "{\"0:ARR-OBJ\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        snprintf(buf, sizeof(buf), "%s{\"1:U16\":%u,\"2:U16\":%u}",
                 i ? "," : "", (unsigned)entries[i].group_id, (unsigned)entries[i].keyset_id);
        json += buf;
    }
    json += "]}";
    return json;
}

// Serializes a full ACL list. Subjects are emitted as quoted decimal strings
// (CASE = node ids, Group = group ids) to avoid 64-bit precision loss.
static std::string build_acl_entries_json(const std::vector<acl_entry_t> &entries)
{
    std::string json = "{\"0:ARR-OBJ\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const acl_entry_t &e = entries[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s{\"1:U8\":%u,\"2:U8\":%u,\"3:ARR-U64\":[",
                 i ? "," : "", (unsigned)e.privilege, (unsigned)e.auth_mode);
        json += buf;
        for (size_t j = 0; j < e.subjects.size(); ++j) {
            snprintf(buf, sizeof(buf), "%s\"%llu\"", j ? "," : "", (unsigned long long)e.subjects[j]);
            json += buf;
        }
        json += "],\"4:NULL\":null}";
    }
    json += "]}";
    return json;
}

// Installs the group epoch key on a node (KeySetWrite) and ensures its GroupKeyMap
// maps group_id -> keyset_id. Shared by add_group_member and create_group_binding.
static esp_err_t install_group_key_on_node(uint64_t node_id, uint16_t group_id, uint16_t keyset_id)
{
    uint8_t key[kEpochKeyLen];
    if (!keyset_key_load(keyset_id, key)) {
        ESP_LOGE(TAG, "No stored epoch key for keyset %u", (unsigned)keyset_id);
        return ESP_ERR_NOT_FOUND;
    }
    unsigned char b64[32];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, key, kEpochKeyLen) != 0)
        return ESP_FAIL;

    // KeySetWrite (GroupKeySetStruct): EpochKey0 = base64 octstr, EpochStartTime0 >= 1,
    // remaining epoch slots null. Policy = TrustFirst.
    char data[192];
    snprintf(data, sizeof(data),
             "{\"0:OBJ\":{\"0:U16\":%u,\"1:U8\":%u,\"2:BYT\":\"%.*s\",\"3:U64\":\"1\","
             "\"4:NULL\":null,\"5:NULL\":null,\"6:NULL\":null,\"7:NULL\":null}}",
             (unsigned)keyset_id, (unsigned)kGroupKeyPolicyTrustFirst, (int)b64_len, b64);
    ESP_LOGI(TAG, "KeySetWrite keyset %u on node 0x%llx: %s", (unsigned)keyset_id, (unsigned long long)node_id, data);
    esp_err_t err = blocking_invoke_cmd(node_id, 0, kGroupKeyMgmtCluster, kKeySetWriteCommand, data);
    if (err != ESP_OK)
        return err;

    // GroupKeyMap read-modify-write so existing maps on the node are preserved.
    err = blocking_read_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupKeyMapAttribute,
                             on_groupkeymap_read_attr);
    std::vector<group_key_map_t> entries = (err == ESP_OK) ? s_group_key_map : std::vector<group_key_map_t>{};
    bool exists = false;
    for (const auto &e : entries)
        if (e.group_id == group_id) { exists = true; break; }
    if (!exists)
        entries.push_back({group_id, keyset_id});
    std::string json = build_groupkeymap_json(entries);
    ESP_LOGI(TAG, "GroupKeyMap write on node 0x%llx: %s", (unsigned long long)node_id, json.c_str());
    return blocking_write_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupKeyMapAttribute, json.c_str());
}

esp_err_t matter_controller_create_group(uint16_t group_id, const char *name)
{
    // Lazily provision the single shared application keyset: generate + persist a
    // random epoch key the first time, reuse it forever after (regenerating would
    // invalidate the key already installed on existing members).
    uint8_t key[kEpochKeyLen];
    if (!keyset_key_load(kAppKeysetId, key)) {
        esp_fill_random(key, sizeof(key));
        esp_err_t store_err = keyset_key_store(kAppKeysetId, key);
        if (store_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist shared epoch key");
            return store_err;
        }
    }
    char key_hex[2 * kEpochKeyLen + 1];
    bytes_to_hex(key, kEpochKeyLen, key_hex);

    char name_buf[32];
    snprintf(name_buf, sizeof(name_buf), "%s", (name && *name) ? name : "Group");

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    esp_err_t err = esp_matter::controller::group_settings::add_keyset(kAppKeysetId, kGroupKeyPolicyTrustFirst, 0, key_hex);
    
    if (err == ESP_OK)
    {
        err = esp_matter::controller::group_settings::add_group(name_buf, group_id);
    }

    if (err == ESP_OK)
    {
        err = esp_matter::controller::group_settings::bind_keyset(group_id, kAppKeysetId);
    }
    
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK)
        ESP_LOGE(TAG, "create_group %u failed: 0x%x", (unsigned)group_id, err);
    else
        ESP_LOGI(TAG, "Created group %u (shared keyset %u)", (unsigned)group_id, (unsigned)kAppKeysetId);
    return err;
}

esp_err_t matter_controller_delete_group(uint16_t group_id)
{
    // Unbind this group from the shared keyset and drop the group info, but keep
    // the shared keyset itself — the other groups still reference it.
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_matter::controller::group_settings::unbind_keyset(group_id, kAppKeysetId);
    esp_matter::controller::group_settings::remove_group(group_id);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    ESP_LOGI(TAG, "Deleted group %u", (unsigned)group_id);
    return ESP_OK;
}

esp_err_t matter_controller_add_group_member(uint64_t node_id, uint16_t group_id, const char *name)
{
    ESP_LOGI(TAG, "add_group_member: node 0x%llx -> group %u (shared keyset %u)",
             (unsigned long long)node_id, (unsigned)group_id, (unsigned)kAppKeysetId);

    // 1. Install the shared group key + GroupKeyMap on the member.
    esp_err_t err = install_group_key_on_node(node_id, group_id, kAppKeysetId);
    if (err != ESP_OK)
        return err;

    // 2. Grant the group Operate on the member's ACL (read-modify-write of the full
    //    list). The controller's Administer/CASE entry is reconstructed canonically;
    //    every other existing entry is preserved.
    err = blocking_read_attr(node_id, 0, kAclCluster, kAclAttribute, on_acl_read_full);
    if (err != ESP_OK)
        return err;
    std::vector<acl_entry_t> entries;
    entries.push_back({kAclPrivilegeAdminister, kAclAuthModeCase, {kControllerNodeId}});
    bool group_present = false;
    for (const acl_entry_t &e : s_acl_entries) {
        if (e.privilege == kAclPrivilegeAdminister && e.auth_mode == kAclAuthModeCase)
            continue;  // folded into the canonical controller entry above
        if (e.privilege == kAclPrivilegeOperate && e.auth_mode == kAclAuthModeGroup) {
            acl_entry_t g = e;
            if (std::find(g.subjects.begin(), g.subjects.end(), group_id) == g.subjects.end())
                g.subjects.push_back(group_id);
            entries.push_back(std::move(g));
            group_present = true;
        } else {
            entries.push_back(e);
        }
    }
    if (!group_present)
        entries.push_back({kAclPrivilegeOperate, kAclAuthModeGroup, {group_id}});
    std::string acl_json = build_acl_entries_json(entries);
    ESP_LOGI(TAG, "add_group_member: writing ACL to node 0x%llx (%u entries): %s",
             (unsigned long long)node_id, (unsigned)entries.size(), acl_json.c_str());
    err = blocking_write_attr(node_id, 0, kAclCluster, kAclAttribute, acl_json.c_str());
    if (err != ESP_OK)
        return err;

    // 3. AddGroup on the device's application (On/Off) endpoint.
    uint16_t endpoint = 1;
    device_manager_get_onoff_endpoint(node_id, &endpoint);
    char data[64];
    snprintf(data, sizeof(data), "{\"0:U16\":%u,\"1:STR\":\"%s\"}",
             (unsigned)group_id, (name && *name) ? name : "");
    err = blocking_invoke_cmd(node_id, endpoint, kGroupsCluster, kAddGroupCommand, data);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "Added node 0x%llx to group %u", (unsigned long long)node_id, (unsigned)group_id);
    return ESP_OK;
}

esp_err_t matter_controller_remove_group_member(uint64_t node_id, uint16_t group_id)
{
    // 1. RemoveGroup on the application endpoint.
    uint16_t endpoint = 1;
    device_manager_get_onoff_endpoint(node_id, &endpoint);
    char data[32];
    snprintf(data, sizeof(data), "{\"0:U16\":%u}", (unsigned)group_id);
    blocking_invoke_cmd(node_id, endpoint, kGroupsCluster, kRemoveGroupCommand, data);

    // 2. Drop the group's GroupKeyMap entry (best effort RMW).
    if (blocking_read_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupKeyMapAttribute,
                           on_groupkeymap_read_attr) == ESP_OK) {
        std::vector<group_key_map_t> entries;
        for (const auto &e : s_group_key_map)
            if (e.group_id != group_id)
                entries.push_back(e);
        std::string json = build_groupkeymap_json(entries);
        blocking_write_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupKeyMapAttribute, json.c_str());
    }

    // 3. Drop the group id from the ACL Group/Operate entry (best effort RMW).
    if (blocking_read_attr(node_id, 0, kAclCluster, kAclAttribute, on_acl_read_full) == ESP_OK) {
        std::vector<acl_entry_t> entries;
        entries.push_back({kAclPrivilegeAdminister, kAclAuthModeCase, {kControllerNodeId}});
        for (const acl_entry_t &e : s_acl_entries) {
            if (e.privilege == 0 || e.auth_mode == 0)
                continue;  // drop invalid/empty entries (see add_group_member)
            if (e.privilege == kAclPrivilegeAdminister && e.auth_mode == kAclAuthModeCase)
                continue;
            acl_entry_t out = e;
            if (e.privilege == kAclPrivilegeOperate && e.auth_mode == kAclAuthModeGroup) {
                out.subjects.clear();
                for (uint64_t s : e.subjects)
                    if (s != group_id)
                        out.subjects.push_back(s);
                if (out.subjects.empty())
                    continue;  // drop an empty group entry
            }
            entries.push_back(std::move(out));
        }
        std::string acl_json = build_acl_entries_json(entries);
        blocking_write_attr(node_id, 0, kAclCluster, kAclAttribute, acl_json.c_str());
    }

    ESP_LOGI(TAG, "Removed node 0x%llx from group %u", (unsigned long long)node_id, (unsigned)group_id);
    return ESP_OK;
}

esp_err_t matter_controller_create_group_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                                 uint16_t group_id)
{
    if (switch_endpoint == 0 &&
        device_manager_get_switch_endpoint(switch_node_id, &switch_endpoint) != ESP_OK) {
        ESP_LOGW(TAG, "No switch endpoint for node 0x%llx", (unsigned long long)switch_node_id);
        return ESP_ERR_NOT_FOUND;
    }

    // The switch sends groupcast, so it needs the shared group key installed too.
    esp_err_t err = install_group_key_on_node(switch_node_id, group_id, kAppKeysetId);
    if (err != ESP_OK)
        return err;

    // Add a group target to the switch's Binding list (read-modify-write).
    err = blocking_read_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                             on_binding_read_attr);
    std::vector<binding_target_t> targets = (err == ESP_OK) ? s_binding_entries : std::vector<binding_target_t>{};
    bool exists = false;
    for (const auto &t : targets)
        if (t.is_group && t.group == group_id && t.cluster == kOnOffCluster) { exists = true; break; }
    if (!exists) {
        binding_target_t t = {0, 0, kOnOffCluster, group_id, true};
        targets.push_back(t);
    }
    std::string binding_json = build_binding_json(targets);
    err = blocking_write_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                              binding_json.c_str());
    if (err != ESP_OK)
        return err;

    uint16_t groups_endpoint = 0;
    if (device_manager_get_onoff_endpoint(switch_node_id, &groups_endpoint) == ESP_OK) {
        char gdata[64];
        snprintf(gdata, sizeof(gdata), "{\"0:U16\":%u,\"1:STR\":\"\"}", (unsigned)group_id);
        esp_err_t aerr = blocking_invoke_cmd(switch_node_id, groups_endpoint, kGroupsCluster,
                                             kAddGroupCommand, gdata);
        if (aerr != ESP_OK)
            ESP_LOGW(TAG, "AddGroup on switch 0x%llx ep%u failed: 0x%x (group send may fail NOT_FOUND)",
                     (unsigned long long)switch_node_id, groups_endpoint, aerr);
    } else {
        ESP_LOGW(TAG, "Switch 0x%llx has no Groups-server endpoint; outgoing group send will fail NOT_FOUND",
                 (unsigned long long)switch_node_id);
    }

    ESP_LOGI(TAG, "Created group binding switch 0x%llx ep%u -> group %u",
             (unsigned long long)switch_node_id, switch_endpoint, (unsigned)group_id);
    return ESP_OK;
}

esp_err_t matter_controller_delete_group_binding(uint64_t switch_node_id, uint16_t switch_endpoint,
                                                 uint16_t group_id)
{
    if (switch_endpoint == 0 &&
        device_manager_get_switch_endpoint(switch_node_id, &switch_endpoint) != ESP_OK)
        return ESP_ERR_NOT_FOUND;

    if (blocking_read_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                           on_binding_read_attr) == ESP_OK) {
        std::vector<binding_target_t> targets;
        for (const auto &t : s_binding_entries)
            if (!(t.is_group && t.group == group_id && t.cluster == kOnOffCluster))
                targets.push_back(t);
        std::string binding_json = build_binding_json(targets);
        blocking_write_attr(switch_node_id, switch_endpoint, kBindingCluster, kBindingAttribute,
                            binding_json.c_str());
    }

    // Undo the GroupInfo membership that create_group_binding added (RemoveGroup on the
    // switch's Groups-server endpoint), so the switch's local load no longer follows it.
    uint16_t groups_endpoint = 0;
    if (device_manager_get_onoff_endpoint(switch_node_id, &groups_endpoint) == ESP_OK) {
        char gdata[32];
        snprintf(gdata, sizeof(gdata), "{\"0:U16\":%u}", (unsigned)group_id);
        blocking_invoke_cmd(switch_node_id, groups_endpoint, kGroupsCluster, kRemoveGroupCommand, gdata);
    }

    ESP_LOGI(TAG, "Deleted group binding switch 0x%llx -> group %u",
             (unsigned long long)switch_node_id, (unsigned)group_id);
    return ESP_OK;
}

esp_err_t matter_controller_reset_node_groups(uint64_t node_id)
{
    ESP_LOGI(TAG, "reset_node_groups: clearing all group state on node 0x%llx",
             (unsigned long long)node_id);

    // 1. Empty the GroupKeyMap so no group references a keyset.
    std::string empty_map = build_groupkeymap_json({});
    blocking_write_attr(node_id, 0, kGroupKeyMgmtCluster, kGroupKeyMapAttribute, empty_map.c_str());

    // 2. Remove every app keyset (the IPK, keyset 0, is left intact). KeySetRemove of a
    //    non-existent id just returns NOT_FOUND, which is harmless here.
    for (uint16_t ks = 1; ks <= kMaxGroupKeysetScan; ++ks) {
        char data[32];
        snprintf(data, sizeof(data), "{\"0:U16\":%u}", (unsigned)ks);
        blocking_invoke_cmd(node_id, 0, kGroupKeyMgmtCluster, kKeySetRemoveCommand, data);
    }

    // 3. RemoveAllGroups on the application endpoint (Groups cluster, no fields).
    uint16_t endpoint = 1;
    device_manager_get_onoff_endpoint(node_id, &endpoint);
    blocking_invoke_cmd(node_id, endpoint, kGroupsCluster, kRemoveAllGroupsCommand, "{}");

    ESP_LOGI(TAG, "reset_node_groups: done for node 0x%llx", (unsigned long long)node_id);
    return ESP_OK;
}

esp_err_t matter_controller_reset_acl(uint64_t node_id)
{
    // Rebuild the ACL with only the controller's Administer/CASE entry (no Operate
    // subjects), which replaces the whole list and drops everything else.
    std::string acl_json = build_acl_json(kControllerNodeId, {});
    ESP_LOGI(TAG, "reset_acl: writing ACL to node 0x%llx: %s",
             (unsigned long long)node_id, acl_json.c_str());
    esp_err_t err = blocking_write_attr(node_id, 0, kAclCluster, kAclAttribute, acl_json.c_str());
    if (err != ESP_OK)
        ESP_LOGE(TAG, "reset_acl: write to node 0x%llx failed: 0x%x", (unsigned long long)node_id, err);
    return err;
}

esp_err_t matter_controller_reset_bindings(uint64_t node_id)
{
    // The Binding cluster lives on the device's switch endpoint; devices without one
    // hold no bindings, so there is nothing to clear.
    uint16_t endpoint = 0;
    if (device_manager_get_switch_endpoint(node_id, &endpoint) != ESP_OK) {
        ESP_LOGI(TAG, "reset_bindings: node 0x%llx has no switch endpoint, nothing to clear",
                 (unsigned long long)node_id);
        return ESP_OK;
    }
    // Empty binding list replaces the whole table.
    std::string binding_json = build_binding_json({});
    ESP_LOGI(TAG, "reset_bindings: clearing Binding table on node 0x%llx ep%u",
             (unsigned long long)node_id, endpoint);
    esp_err_t err = blocking_write_attr(node_id, endpoint, kBindingCluster, kBindingAttribute,
                                        binding_json.c_str());
    if (err != ESP_OK)
        ESP_LOGE(TAG, "reset_bindings: write to node 0x%llx failed: 0x%x",
                 (unsigned long long)node_id, err);
    return err;
}

esp_err_t matter_controller_groupcast_toggle(uint16_t group_id)
{
    // A Matter group node id is 0xFFFFFFFFFFFF0000 | group_id (chip::IsGroupId range);
    // send_invoke_cluster_command routes this as a groupcast. Endpoint is ignored.
    // Toggle is state-independent and matches what the bound switch sends.
    uint64_t dest = 0xFFFFFFFFFFFF0000ULL | (uint64_t)group_id;
    ESP_LOGI(TAG, "groupcast OnOff Toggle to group %u", (unsigned)group_id);
    return blocking_invoke_cmd(dest, 0, kOnOffCluster, kOnOffCmdToggle, "{}");
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

    // Persistent semaphore (created once): the commissioning success/failure callback may fire
    // after this function has returned (slow attestation/revocation), so it must not be deleted.
    if (!s_commission_ctx.done) {
        s_commission_ctx.done = xSemaphoreCreateBinary();
        if (!s_commission_ctx.done)
            return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_commission_ctx.done, 0);  // drain any stale signal from a prior attempt
    s_commission_ctx.result = CHIP_NO_ERROR;

    chip::NodeId node_id = matter_controller_allocate_node_id();

    matter_controller::controller::pairing_command_callbacks_t callbacks = {
        .commissioning_success_callback = on_commissioning_success_callback,
        .commissioning_failure_callback = on_commissioning_failure_callback,
    };
    matter_controller::controller::pairing_command::get_instance().set_callbacks(callbacks);

    ESP_LOGI(TAG, "Attempting to commission node %llu", node_id);

    // TODO(thread): when commissioning a Thread device (no on-network path
    // available), switch to a BLE->Thread pairing flow and attach the shared
    // Thread operational dataset retrieved via Thread Credential Sharing. The
    // dataset is available from thread_credentials_get_dataset() once the
    // controller has pulled it from a Border Router (see thread_credentials.h).
    // Requires enabling BLE (CONFIG_BT_ENABLED + CHIP BLE) — deferred.

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    //esp_matter::controller::pairing_code(node_id, onboarding_payload);
    matter_controller::controller::pairing_on_network(node_id, payload.setUpPINCode);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    // Attestation (DCL PAA lookup + revocation check) can take well over a minute on an
    // offline controller, so allow a generous window before giving up.
    if (xSemaphoreTake(s_commission_ctx.done, pdMS_TO_TICKS(180000)) != pdTRUE) {
        ESP_LOGE(TAG, "Commissioning timed out");
        return ESP_ERR_TIMEOUT;
    }

    CHIP_ERROR result = s_commission_ctx.result;

    if (result == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Processing new node %llu", node_id);

        if (node_id_out)
            *node_id_out = (uint64_t)node_id;
        node_list_add(node_id);

        // Save the device and its canvas node immediately, so it is recorded even if
        // interrogation is slow or fails. Interrogation then enriches it (vendor/product/
        // device type/endpoints) and updates the saved node when it completes.
        device_manager_add_device(node_id);
        device_manager_persist();
        add_canvas_node(node_id, 0);  // device type unknown until interrogation enriches it

        if (s_interrogation_done)
            xSemaphoreTake(s_interrogation_done, 0);
        interrogate_node(node_id);
        if (s_interrogation_done &&
            xSemaphoreTake(s_interrogation_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGW(TAG, "Interrogation did not finish in time for node 0x%llx; saved with minimal info",
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
    // Drop the stale endpoint structure so the refresh reflects the device as it
    // is now (endpoints/types that disappeared are not re-added by the read).
    device_manager_clear_device_endpoints(node_id);

    // Drain any leftover completion signal (commissioning fires interrogation
    // without consuming it), then block until this interrogation finishes so the
    // caller sees freshly persisted data.
    if (s_interrogation_done)
        xSemaphoreTake(s_interrogation_done, 0);

    interrogate_node(node_id);

    if (s_interrogation_done &&
        xSemaphoreTake(s_interrogation_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGW(TAG, "Re-interview of node 0x%llx timed out", (unsigned long long)node_id);
        return ESP_ERR_TIMEOUT;
    }
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
