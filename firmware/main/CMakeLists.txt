idf_component_register(SRCS
        "main.cpp"
        "web_server.c"
        "ws_server.c"
        "matter_controller.cpp"
        "matter_ethernet_driver.cpp"
        "node_manager.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils"
    PRIV_REQUIRES json esp_eth driver nvs_flash button esp_matter esp_matter_controller esp_http_server
    LDFRAGMENTS "linker.lf"
)

if(CONFIG_SPIFFS_ATTESTATION_TRUST_STORE)
    spiffs_create_partition_image(paa_cert ${CMAKE_SOURCE_DIR}/paa_cert FLASH_IN_PROJECT)
endif()

add_custom_target(react_build
    COMMAND npm run build --emptyOutDir
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/html_app
    COMMENT "Building React app..."
)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/html_compiled_app" FLASH_IN_PROJECT DEPENDS react_build)

# Replace the default Matter Ethernet commissioning driver with our W5500 implementation.
# The default driver is excluded by marking it header-only; matter_ethernet_driver.cpp
# provides the ESPEthernetDriver::Init() implementation instead.
idf_component_get_property(matter_lib esp_matter COMPONENT_LIB)
set_source_files_properties(
    "${MATTER_SDK_PATH}/config/esp32/third_party/connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver_Ethernet.cpp"
    TARGET_DIRECTORY ${matter_lib}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
