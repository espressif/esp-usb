idf_component_register(
    SRCS "src/esp_typec.c" "src/esp_typec_pd.c" "src/fusb302_ctrl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include_private"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c
    REQUIRES freertos
)
