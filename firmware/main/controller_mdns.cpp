#include "controller_mdns.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
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
        esp_ip6_addr_type_t t = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        const char *kind = t == ESP_IP6_ADDR_IS_LINK_LOCAL   ? "link-local"
                         : t == ESP_IP6_ADDR_IS_GLOBAL        ? "global"
                         : t == ESP_IP6_ADDR_IS_UNIQUE_LOCAL  ? "unique-local"
                         : t == ESP_IP6_ADDR_IS_SITE_LOCAL    ? "site-local"
                         : "other";
        ESP_LOGI(TAG, "GOT IP6 [%s]: " IPV6STR, kind, IPV62STR(event->ip6_info.ip));
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
