# NOTE: This kind of mocking currently works on Linux targets only.
#       On Espressif chips, too many dependencies are missing at the moment.

# USBH Layer mock
message(STATUS "building HCD Layer MOCKS")

# Get USB component dir
idf_component_get_property(original_usb_dir usb COMPONENT_OVERRIDEN_DIR)

# Get USB HAL component dir
idf_component_get_property(original_usb_hal_dir esp_hal_usb COMPONENT_OVERRIDEN_DIR)
if(NOT original_usb_hal_dir)
    idf_component_get_property(original_usb_hal_dir esp_hal_usb COMPONENT_DIR)
endif()

# Get esp hw support component dir
idf_component_get_property(original_esp_hw_support_dir esp_hw_support COMPONENT_OVERRIDEN_DIR)
if(NOT original_esp_hw_support_dir)
    idf_component_get_property(original_esp_hw_support_dir esp_hw_support COMPONENT_DIR)
endif()

idf_component_mock(INCLUDE_DIRS "${original_usb_dir}/include"
                                "${original_usb_dir}/include/usb"
                                "${original_usb_dir}/private_include"
                                "${original_usb_hal_dir}/include/hal"       # usb_dwc_hal.h
                                "${original_usb_hal_dir}/include"           # hal/usb_dwc_types.h
                                "${original_usb_hal_dir}/esp32p4/include"   # hal/usb_dwc_ll.h
                                "${CMAKE_CURRENT_LIST_DIR}/include"         # local soc_caps.h and usb_dwc_struct.h
                   MOCK_HEADER_FILES ${original_usb_hal_dir}/include/hal/usb_dwc_hal.h
                                     ${original_usb_dir}/private_include/usb_private.h
                                     ${original_esp_hw_support_dir}/include/esp_intr_alloc.h
                   REQUIRES freertos)

# This definition is missing for linux target, so we add it here
target_compile_definitions(${COMPONENT_LIB} PRIVATE -DSOC_USB_OTG_PERIPH_NUM=2)
target_compile_definitions(${COMPONENT_LIB} PRIVATE -DSOC_USB_OTG_SUPPORTED=1)

# The DWC HAL/LL headers cast between 32-bit integers and pointers, which triggers
# warnings (errors with -Werror) when building for the 64-bit linux target
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)

# We do not mock hcd_dwc.c, we use the original implementation of it
target_sources(${COMPONENT_LIB} PRIVATE "${original_usb_dir}/src/hcd_dwc.c")

# Dummy usb_dwc_info peripheral info. CMock only mocks functions, not data symbols.
# On real chips this is defined in the target-specific usb_dwc_periph.c (esp_hal_usb component).
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb_dwc_periph.c")

# Test helpers: set up HAL mock expectations from C code, so the C++ test cases
# do not have to include the DWC HAL/LL headers (they contain 32-bit pointer casts
# that do not compile as C++ on the 64-bit linux target)
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/hcd_test_helpers.c")
