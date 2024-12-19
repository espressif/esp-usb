cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)

list(APPEND EXTRA_COMPONENT_DIRS
     "$ENV{IDF_PATH}/tools/mocks/usb/"
    )
add_definitions("-DCMOCK_MEM_DYNAMIC") # We need a lot of memory for our frame buffers
project(host_test_usb_uvc)
