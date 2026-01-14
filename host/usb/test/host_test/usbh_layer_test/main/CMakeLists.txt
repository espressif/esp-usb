set(srcs)
list(APPEND srcs "test_main.cpp"
                 "usbh_install_unit_test.cpp"
                 )

idf_component_register(SRCS  ${srcs}
                        REQUIRES cmock usb
                        WHOLE_ARCHIVE)

# We do not use main() from esp-idf as it discards argc and argv arguments
# We wrap main so argc and argv are correctly passed to Catch2
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=main")
