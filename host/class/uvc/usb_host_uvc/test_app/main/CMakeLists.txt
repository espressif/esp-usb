include($ENV{IDF_PATH}/tools/cmake/version.cmake)
set (TINYUSB_LIB)
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.0")
    set(TINYUSB_LIB "esp_tinyusb")
else()
    set(TINYUSB_LIB "tinyusb")
endif()

# TODO: once IDF_v4.4 is at the EOL support, use WHOLE_ARCHIVE
idf_component_register(SRCS "test_uvc.c" "libusb_parse.c" "test_app_main.c"
                       INCLUDE_DIRS "." "../../private_include"
                       REQUIRES usb_host_uvc usb unity)

# In order for the cases defined by `TEST_CASE` to be linked into the final elf,
# the component can be registered as WHOLE_ARCHIVE

# Due to the backward compatibility to IDFv4.4 (in which WHOLE_ARCHIVE is not implemented) we use following approach:
#   Any non-static function from test_app/main/*.c (apart from test_app_main.c) file is added as an undefined symbol
#   because otherwise the linker will ignore test_app/main/*.c as it has no other files depending on any
#   symbols in it.

# force-link test_uvc.c -  void function linker_hook created because:
# no non-static function
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-u linker_hook")
