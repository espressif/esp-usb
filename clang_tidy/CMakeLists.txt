# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/version.cmake)

# Specify components that will be checked with Clang tidy
set(EXTRA_COMPONENT_DIRS
    ../device/esp_tinyusb
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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(clang_check)
