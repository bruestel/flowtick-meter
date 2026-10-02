idf_component_register(
    SRCS "src/Meter.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc esp_driver_gpio esp_timer nvs_flash espressif__cjson
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
