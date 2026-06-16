#include "controller_mdns.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip6_addr.h"
#include "mdns.h"

static const char *TAG = "controller_mdns";

// Advertised as "matter-controller.local" (mDNS appends the .local domain).
#define CONTROLLER_MDNS_HOSTNAME "matter-controller"

// Adds the delegated hostname on first sight, then keeps its address current.
static void publish_address(const esp_ip_addr_t *addr)
{
    mdns_ip_addr_t node = {};
    node.addr = *addr;
    node.next = nullptr;

    esp_err_t err;
    if (mdns_hostname_exists(CONTROLLER_MDNS_HOSTNAME)) {
        err = mdns_delegate_hostname_set_address(CONTROLLER_MDNS_HOSTNAME, &node);
    } else {
        err = mdns_delegate_hostname_add(CONTROLLER_MDNS_HOSTNAME, &node);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish %s.local: 0x%x", CONTROLLER_MDNS_HOSTNAME, err);
    } else {
        ESP_LOGI(TAG, "Advertising %s.local", CONTROLLER_MDNS_HOSTNAME);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        esp_ip_addr_t addr = {};
        addr.type = ESP_IPADDR_TYPE_V4;
        addr.u_addr.ip4 = event->ip_info.ip;
        publish_address(&addr);
    } else if (id == IP_EVENT_GOT_IP6) {
        auto *event = static_cast<ip_event_got_ip6_t *>(data);
        esp_ip_addr_t addr = {};
        addr.type = ESP_IPADDR_TYPE_V6;
        addr.u_addr.ip6 = event->ip6_info.ip;
        publish_address(&addr);
    }
}

esp_err_t controller_mdns_start(void)
{
    // mDNS may already be running (the Matter stack uses the same component on
    // Ethernet-only builds); INVALID_STATE means it is already initialised.
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mdns_init failed: 0x%x", err);
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: 0x%x", err);
        return err;
    }

    // Cover the case where Ethernet already obtained an IP before we registered.
    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (eth && esp_netif_get_ip_info(eth, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        esp_ip_addr_t addr = {};
        addr.type = ESP_IPADDR_TYPE_V4;
        addr.u_addr.ip4 = ip_info.ip;
        publish_address(&addr);
    }

    return ESP_OK;
}

void controller_mdns_probe_commissionable(uint32_t timeout_ms)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "probe: mdns_init failed: 0x%x", err);
        return;
    }

    ESP_LOGI(TAG, "probe: browsing _matterc._udp for %u ms ...", timeout_ms);
    mdns_result_t *results = nullptr;
    err = mdns_query_ptr("_matterc", "_udp", timeout_ms, 20, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe: mdns_query_ptr failed: 0x%x", err);
        return;
    }

    size_t count = 0;
    for (mdns_result_t *r = results; r != nullptr; r = r->next, count++) {
        const char *ifkey = r->esp_netif ? esp_netif_get_ifkey(r->esp_netif) : "(null)";
        ESP_LOGI(TAG, "probe: result[%u] instance='%s' host='%s' port=%u iface=%s",
                 (unsigned)count,
                 r->instance_name ? r->instance_name : "(null)",
                 r->hostname ? r->hostname : "(null)",
                 r->port,
                 ifkey ? ifkey : "(null)");
        for (mdns_ip_addr_t *a = r->addr; a != nullptr; a = a->next) {
            char ip[46] = {0};
            if (a->addr.type == ESP_IPADDR_TYPE_V4)
                esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip, sizeof(ip));
            else
                ip6addr_ntoa_r((const ip6_addr_t *)&a->addr.u_addr.ip6, ip, sizeof(ip));
            ESP_LOGI(TAG, "probe:   addr %s (%s)", ip,
                     a->addr.type == ESP_IPADDR_TYPE_V4 ? "v4" : "v6");
        }
        for (size_t t = 0; t < r->txt_count; t++) {
            ESP_LOGI(TAG, "probe:   txt %s=%s",
                     r->txt[t].key ? r->txt[t].key : "",
                     r->txt[t].value ? r->txt[t].value : "");
        }
    }
    ESP_LOGI(TAG, "probe: %u commissionable node(s) seen via mDNS", (unsigned)count);
    mdns_query_results_free(results);
}
