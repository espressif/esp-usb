set(srcs)
list(APPEND srcs "test_main.cpp"
                 "usbh_install_unit_test.cpp"
                 "usbh_control_completion_test.cpp"
                 )

idf_component_register(SRCS  ${srcs}
                        REQUIRES cmock usb
                        WHOLE_ARCHIVE)
