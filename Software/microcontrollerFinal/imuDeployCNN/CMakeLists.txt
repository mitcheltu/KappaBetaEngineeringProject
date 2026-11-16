cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/arduino_libraries")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Match the existing Arduino build's USB CDC and PSRAM board flags.
idf_build_set_property(COMPILE_DEFINITIONS
    "BOARD_HAS_PSRAM=1;ARDUINO_USB_MODE=0;ARDUINO_USB_CDC_ON_BOOT=1"
    APPEND)

project(imu_deploy_cnn)
