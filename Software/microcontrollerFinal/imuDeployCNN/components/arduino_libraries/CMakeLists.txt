if(NOT DEFINED ARDUINO_LIBRARIES_DIR OR ARDUINO_LIBRARIES_DIR STREQUAL "")
    if(DEFINED ENV{ARDUINO_LIBRARIES_DIR} AND
       NOT "$ENV{ARDUINO_LIBRARIES_DIR}" STREQUAL "")
        set(ARDUINO_LIBRARIES_DIR "$ENV{ARDUINO_LIBRARIES_DIR}")
    else()
        set(ARDUINO_LIBRARIES_DIR "$ENV{HOME}/Documents/Arduino/libraries")
    endif()
endif()

function(require_arduino_library folder expected_version output_var)
    set(library_dir "${ARDUINO_LIBRARIES_DIR}/${folder}")
    if(NOT EXISTS "${library_dir}/library.properties")
        message(FATAL_ERROR
            "Missing Arduino library ${folder}. Install the pinned libraries or set ARDUINO_LIBRARIES_DIR.")
    endif()

    file(STRINGS "${library_dir}/library.properties" version_line
         REGEX "^version=")
    if(NOT "${version_line}" STREQUAL "version=${expected_version}")
        message(FATAL_ERROR
            "${folder} must be version ${expected_version}; found '${version_line}'.")
    endif()

    set(${output_var} "${library_dir}" PARENT_SCOPE)
endfunction()

require_arduino_library(
    "BlueRobotics_MS5837_Library" "1.1.1" MS5837_DIR)
require_arduino_library(
    "NimBLE-Arduino" "2.5.2" NIMBLE_DIR)
require_arduino_library(
    "SparkFun_MAX3010x_Pulse_and_Proximity_Sensor_Library" "1.1.2" MAX3010X_DIR)

file(GLOB_RECURSE NIMBLE_SOURCES CONFIGURE_DEPENDS
    "${NIMBLE_DIR}/src/*.c"
    "${NIMBLE_DIR}/src/*.cpp")
file(GLOB_RECURSE NIMBLE_PATHS LIST_DIRECTORIES TRUE
    "${NIMBLE_DIR}/src/*")
set(NIMBLE_INCLUDE_DIRS "${NIMBLE_DIR}/src")
foreach(path IN LISTS NIMBLE_PATHS)
    if(IS_DIRECTORY "${path}")
        list(APPEND NIMBLE_INCLUDE_DIRS "${path}")
    endif()
endforeach()

file(GLOB_RECURSE MAX3010X_SOURCES CONFIGURE_DEPENDS
    "${MAX3010X_DIR}/src/*.c"
    "${MAX3010X_DIR}/src/*.cpp")

if(NOT EXISTS "${MS5837_DIR}/MS5837.cpp" OR
   NOT NIMBLE_SOURCES OR NOT MAX3010X_SOURCES)
    message(FATAL_ERROR "One or more pinned Arduino library source files were not found.")
endif()

idf_component_register(
    SRCS
        "${MS5837_DIR}/MS5837.cpp"
        ${NIMBLE_SOURCES}
        ${MAX3010X_SOURCES}
    INCLUDE_DIRS
        "${MS5837_DIR}"
        "${MAX3010X_DIR}/src"
        ${NIMBLE_INCLUDE_DIRS}
    REQUIRES
        arduino
        bt
        esp_timer
        nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ARDUINO=10819
    ARDUINO_ARCH_ESP32=1
)
