set(srcs)
list(APPEND srcs "test_main.cpp"
                 "usb_host_install_unit_test.cpp"
                 "usb_host_install_config_unit_test.cpp"
                 "usb_helpers_descriptor_parsing_test.cpp"
                 "usb_host_client_events_unit_test.cpp"
                 "usb_host_lib_events_unit_test.cpp"
                 "usb_host_enum_events_unit_test.cpp"
                 "usb_host_hub_events_unit_test.cpp"
                 "usb_host_usbh_events_unit_test.cpp"
                 )

idf_component_register(SRCS  ${srcs}
                        REQUIRES cmock usb
                        WHOLE_ARCHIVE)
