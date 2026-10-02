idf_component_register(
    SRCS "main.cpp" "indicator.cpp" "recovery.cpp" "probation.cpp" "diagnostics.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_app_format app_update spi_flash espcoredump esp_partition
             esp_wifi esp_netif espressif__cjson
             app_config app_net app_web app_mqtt meter edgelog
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
