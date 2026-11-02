cmake_minimum_required(VERSION 3.16)

project(kappa_beta_sensor_fusion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sensor_fusion STATIC
    src/sensor_fusion.cpp
)
target_include_directories(sensor_fusion PUBLIC src)

add_executable(sensor_fusion_tests
    tests/test_sensor_fusion.cpp
)
target_link_libraries(sensor_fusion_tests PRIVATE sensor_fusion)

enable_testing()
add_test(NAME sensor_fusion_tests COMMAND sensor_fusion_tests)
