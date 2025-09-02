cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)

# Register usb component, must be registered before registering mock
list(APPEND EXTRA_COMPONENT_DIRS "../../../../usb")

list(APPEND EXTRA_COMPONENT_DIRS
     "../../mocks/usbh_layer_mock/usb"

     # The following line would be needed to include the freertos mock component if this test used mocked FreeRTOS.
     #"$ENV{IDF_PATH}/tools/mocks/freertos/"
    )

project(host_test_usbh_layer)
