# The following boilerplate order is required by ESP-IDF's CMake build system.
cmake_minimum_required(VERSION 3.22)

# This firmware is only intended for ESP32-S3. An explicit IDF_TARGET
# environment variable or idf.py command-line option can still override it.
if(NOT IDF_TARGET AND "$ENV{IDF_TARGET}" STREQUAL "")
    set(IDF_TARGET "esp32s3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(led_blink_test)
