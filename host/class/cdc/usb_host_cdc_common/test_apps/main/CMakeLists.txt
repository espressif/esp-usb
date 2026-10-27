idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES unity usb_host_cdc_common usb esp_timer esp_hw_support heap
                       WHOLE_ARCHIVE)
