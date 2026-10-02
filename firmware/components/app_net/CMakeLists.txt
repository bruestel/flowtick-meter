idf_component_register(
    SRCS "src/Net.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_config esp_wifi esp_netif esp_event esp_timer lwip
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
