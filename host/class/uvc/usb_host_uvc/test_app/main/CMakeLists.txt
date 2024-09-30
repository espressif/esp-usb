idf_component_register(SRCS "test_uvc.c" "libusb_parse.c" "test_app_main.c"
                       INCLUDE_DIRS "." "../../private_include"
                       REQUIRES usb_host_uvc usb unity
                       WHOLE_ARCHIVE)
