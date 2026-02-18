idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES unity
    PRIV_REQUIRES usb_tcpm esp_driver_i2c esp_driver_gpio esp_event
    WHOLE_ARCHIVE
)
