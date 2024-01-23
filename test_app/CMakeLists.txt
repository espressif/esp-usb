# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/version.cmake)

set(EXTRA_COMPONENT_DIRS
        ../host/class/cdc/esp_modem_usb_dte
        ../host/class/cdc/usb_host_cdc_acm
        ../host/class/cdc/usb_host_ch34x_vcp
        ../host/class/cdc/usb_host_cp210x_vcp
        ../host/class/cdc/usb_host_ftdi_vcp
        ../host/class/cdc/usb_host_vcp
        ../host/class/hid/usb_host_hid
        ../host/class/msc/usb_host_msc
        ../host/class/uvc/usb_host_uvc
        )

# Set the components to include the tests for.
set(TEST_COMPONENTS "usb_host_cdc_acm" "usb_host_msc" "usb_host_uvc" "usb_host_hid" CACHE STRING "List of components to test")
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.0")
    list(APPEND EXTRA_COMPONENT_DIRS ../device/esp_tinyusb)
    list(APPEND TEST_COMPONENTS "esp_tinyusb")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_test_app)
