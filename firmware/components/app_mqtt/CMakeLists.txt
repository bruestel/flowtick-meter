idf_component_register(
    SRCS "src/Mqtt.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_config mqtt esp_event mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
