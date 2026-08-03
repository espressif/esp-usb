idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       REQUIRES unity esp_tinyusb
                       PRIV_REQUIRES fatfs wear_levelling esp_partition
                       WHOLE_ARCHIVE)

# Reach esp_tinyusb's private headers to call the MTP test-only helpers.
idf_component_get_property(esp_tinyusb_dir esp_tinyusb COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_tinyusb_dir}/include_private")
