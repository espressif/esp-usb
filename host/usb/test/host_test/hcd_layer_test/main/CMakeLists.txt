set(srcs)
list(APPEND srcs "test_main.cpp"
                 "hcd_install_unit_test.cpp"
                 )

idf_component_register(SRCS  ${srcs}
                        REQUIRES cmock usb
                        WHOLE_ARCHIVE)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DSOC_USB_OTG_SUPPORTED=1)
