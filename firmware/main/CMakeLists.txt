idf_component_register(SRCS
        "main.cpp"
        "web_server.c"
        "ws_server.c"
        "matter_controller.cpp"
        "matter_ethernet_driver.cpp"
        "controller_mdns.cpp"
        "node_manager.cpp"
        "thread_credentials.cpp"
        "thread_meshcop.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES json esp_eth esp_netif driver nvs_flash esp_http_server mdns espressif__esp_matter mbedtls lwip esp_timer
)

add_custom_target(react_build
    COMMAND npm run build --emptyOutDir
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/html_app
    COMMENT "Building React app..."
)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/html_compiled_app" FLASH_IN_PROJECT DEPENDS react_build)

# Replace the default Matter Ethernet commissioning driver with our W5500 implementation.
# The default driver is excluded by marking it header-only; matter_ethernet_driver.cpp
# provides the ESPEthernetDriver::Init() implementation instead.
idf_component_get_property(matter_lib espressif__esp_matter COMPONENT_LIB)
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver_Ethernet.cpp"
    TARGET_DIRECTORY ${matter_lib}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# esp-matter's CMakeLists excludes ESP32DnssdImpl.cpp when Wi-Fi is disabled, but
# DnssdImpl.cpp (included when CONFIG_USE_MINIMAL_MDNS=n) depends on its symbols.
# Add it back so platform mDNS links on Ethernet-only builds.
target_sources(${matter_lib} PRIVATE
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp"
)