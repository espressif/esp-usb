idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       REQUIRES unity esp_tinyusb
                       PRIV_REQUIRES fatfs wear_levelling esp_partition
                       WHOLE_ARCHIVE)
