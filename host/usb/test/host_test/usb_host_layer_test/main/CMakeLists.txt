set(srcs)
list(APPEND srcs "test_main.cpp"
                 "usb_host_install_unit_test.cpp"
                 "usb_helpers_descriptor_parsing_test.cpp"
                 )

idf_component_register(SRCS  ${srcs}
                        REQUIRES cmock usb
                        WHOLE_ARCHIVE)
