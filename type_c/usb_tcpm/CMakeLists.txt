idf_component_register(
    SRCS "src/usb_tcpm.c" "src/fusb302_ctrl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include_private"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c
    REQUIRES esp_event
)
