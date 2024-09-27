idf_component_register(SRCS "test_cdc_acm_host.c" "usb_device.c" "test_app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb_host_cdc_acm unity esp_tinyusb
                       WHOLE_ARCHIVE)
