# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(EXISTS "$ENV{IDF_PATH}/tools/unit-test-app/components")
    list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/unit-test-app/components")
else()
    list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/test_apps/components")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "../../usb_host_cdc_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_host_cdc_common_test)
