cmake_minimum_required(VERSION 3.16)

# Tell IDF where to find the local components
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."            # type_c_manager
    "${CMAKE_CURRENT_LIST_DIR}/../../../type_c"  # type_c
    "${CMAKE_CURRENT_LIST_DIR}/../../../device/esp_tinyusb" # esp_tinyusb
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pd_fusb302_msc_device)

# TinyUSB needs a root hub mode defined; provide a sane default per target.
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "esp32p4")
    set(_tusb_rhport0 "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_HIGH_SPEED)")
else()
    set(_tusb_rhport0 "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)")
endif()
idf_build_set_property(COMPILE_DEFINITIONS "${_tusb_rhport0}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUSB_RHPORT1_MODE=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "BOARD_TUD_RHPORT=0" APPEND)
