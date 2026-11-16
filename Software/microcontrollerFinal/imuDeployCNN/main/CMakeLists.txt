set(FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(FIRMWARE_DIR "${FIRMWARE_DIR}" ABSOLUTE)

idf_component_register(
    SRCS
        "main.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../imu_cnn_model_data.cc"
    INCLUDE_DIRS
        "${FIRMWARE_DIR}"
        "${FIRMWARE_DIR}/imuDeployCNN"
    REQUIRES
        arduino
        arduino_libraries
        esp-tflite-micro
        esp_hw_support
        esp_system
        esp_timer
        freertos
        heap
        nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARDUINO=10819
    ARDUINO_ARCH_ESP32=1
)
