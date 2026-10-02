idf_component_register(
    SRCS "src/EdgeLog.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
