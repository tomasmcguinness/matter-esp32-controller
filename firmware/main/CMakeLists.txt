idf_component_register(SRCS
        "main.cpp"
        "web_server.c"
        "ws_server.c"
        "matter_controller.cpp"
        "controller_mdns.cpp"
        "node_manager.cpp"
        "device_manager.cpp"
        "pairing_command.cpp"
        "thread_credentials.cpp"
        "thread_meshcop.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES json esp_eth esp_netif ethernet_init driver nvs_flash esp_http_server mdns esp_matter esp_matter_controller mbedtls lwip esp_timer
)

add_custom_target(react_build
    COMMAND npm run build --emptyOutDir
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/html_app
    COMMENT "Building React app..."
)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/html_compiled_app" FLASH_IN_PROJECT DEPENDS react_build)