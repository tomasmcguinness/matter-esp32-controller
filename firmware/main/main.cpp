#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "node_manager.h"
#include "matter_controller.h"
#include "thread_credentials.h"
#include "web_server.h"

static const char *TAG = "main";

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(node_manager_init());

    ESP_ERROR_CHECK(thread_credentials_init());

    ESP_ERROR_CHECK(matter_controller_start());

    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "Startup complete");
}
