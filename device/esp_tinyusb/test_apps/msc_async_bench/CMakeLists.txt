# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# No MINIMAL_BUILD: esp_psram must be part of the build so the RAM disk can live in PSRAM

project(test_app_msc_async_bench)
