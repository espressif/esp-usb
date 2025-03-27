cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)

list(APPEND EXTRA_COMPONENT_DIRS
     "$ENV{IDF_PATH}/tools/mocks/usb/usb_host_full_mock/usb/"    # Full USB Host stack mock (all the layers are mocked)
     "$ENV{IDF_PATH}/tools/mocks/freertos/"
    )

add_definitions("-DCMOCK_MEM_DYNAMIC")
project(host_test_usb_cdc)
