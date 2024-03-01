# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
        ../../usb_host_hid
        )

# Set the components to include the tests for.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.0")
    list(APPEND EXTRA_COMPONENT_DIRS ../../../../../device/esp_tinyusb)
endif()

project(test_app_usb_host_hid)