idf_component_register(
    SRCS app_main.c bootloader_update.c diag.c io.c net.c ota.c setup.c update.c
    INCLUDE_DIRS .
    REQUIRES esp_hap_core esp_hap_apple_profiles esp_eth esp_netif esp_event nvs_flash
             app_update esp_http_server esp_adc esp_timer driver mdns esp_partition
             bootloader_support esp_http_client esp_https_ota mbedtls)

# The image carries this build's bootloader (see bootloader_update.c).
idf_build_get_property(build_dir BUILD_DIR)
set_source_files_properties("${build_dir}/bootloader/bootloader.bin" PROPERTIES GENERATED TRUE)
target_add_binary_data(${COMPONENT_LIB} "${build_dir}/bootloader/bootloader.bin" BINARY
                       DEPENDS bootloader)

if(GARAGE_NO_CONFIRM)  # tests only: an image that never confirms, to exercise rollback
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GARAGE_NO_CONFIRM)
endif()
