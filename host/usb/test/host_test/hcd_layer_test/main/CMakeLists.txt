set(srcs)
list(APPEND srcs "test_main.cpp"
                 "hcd_install_unit_test.cpp"
                 )

idf_component_register(SRCS  ${srcs}
                        REQUIRES cmock usb
                        WHOLE_ARCHIVE)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DSOC_USB_OTG_SUPPORTED=1)

# hcd_install_unit_test.cpp includes Mockusb_dwc_hal.h to register HAL mock callbacks.
# The DWC HAL/LL headers cast between 32-bit integers and pointers, which is a hard
# error in C++ on the 64-bit linux target (only a warning in C). For this file only:
# -fpermissive demotes those errors, -w suppresses the resulting warnings.
set_source_files_properties(hcd_install_unit_test.cpp PROPERTIES
                            COMPILE_OPTIONS "-fpermissive;-w")
