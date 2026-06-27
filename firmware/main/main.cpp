#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"

#include "node_manager.h"
#include "device_manager.h"
#include "matter_controller.h"
#include "thread_credentials.h"
#include "controller_mdns.h"
#include "web_server.h"

#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_netif_net_stack.h"

// W5500 SPI Ethernet wiring — matches matter-esp32-modbus-tcp-adapter (same board)
#define ETH_SPI_HOST       SPI2_HOST
#define ETH_SPI_SCLK_GPIO  13
#define ETH_SPI_MOSI_GPIO  11
#define ETH_SPI_MISO_GPIO  12
#define ETH_SPI_CS_GPIO    14
#define ETH_SPI_INT_GPIO   10
#define ETH_SPI_RST_GPIO   9
#define ETH_SPI_CLOCK_MHZ  25

static const char *TAG = "main";

static EventGroupHandle_t s_net_event_group;
#define IPV6_READY_BIT  BIT0
#define SNTP_SYNCED_BIT BIT1

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP sync complete: %lld", (long long)tv->tv_sec);
    bool first = !(xEventGroupGetBits(s_net_event_group) & SNTP_SYNCED_BIT);
    xEventGroupSetBits(s_net_event_group, SNTP_SYNCED_BIT);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(netif);

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        netif_set_flags(lwip_netif, NETIF_FLAG_MLD6);
        esp_netif_create_ip6_linklocal(netif);
        break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Ethernet link down"); break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Ethernet started");   break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Ethernet stopped");   break;
    }
}

static void got_ip6_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    ESP_LOGI(TAG, "Got IPv6: " IPV6STR, IPV62STR(event->ip6_info.ip));

    // struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(event->esp_netif);
    // if (lwip_netif != NULL) {
    //     esp_err_t err = esp_netif_tcpip_exec(join_all_nodes_cb, lwip_netif);
    //     ESP_LOGW(TAG, "mld6_joingroup ff02::1 -> %s", esp_err_to_name(err));
    // }

    xEventGroupSetBits(s_net_event_group, IPV6_READY_BIT);
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();

    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(node_manager_init());

    ESP_ERROR_CHECK(device_manager_init());

    //ESP_ERROR_CHECK(thread_credentials_init());

    // --- Bring up the W5500 directly (no internal EMAC on the S3) ---
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num   = ETH_SPI_MOSI_GPIO;
    buscfg.miso_io_num   = ETH_SPI_MISO_GPIO;
    buscfg.sclk_io_num   = ETH_SPI_SCLK_GPIO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {};
    spi_devcfg.mode           = 0;
    spi_devcfg.clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000;
    spi_devcfg.spics_io_num   = ETH_SPI_CS_GPIO;
    spi_devcfg.queue_size     = 20;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = ETH_SPI_INT_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_SPI_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    uint8_t mac_addr[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, eth_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &got_ip6_event_handler, NULL));

    s_net_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Waiting for IPv6 addresses...");
    xEventGroupWaitBits(s_net_event_group, IPV6_READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));

    ESP_ERROR_CHECK(matter_controller_start());

    ESP_ERROR_CHECK(controller_mdns_start());

    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "Startup complete");
}
