idf_component_register(
    SRCS "src/esp_typec_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        type_c          # for esp_typec_pd.h / esp_typec_pd_*
        esp_event       # for esp_event_handler_register/post
        esp_driver_gpio
        esp_driver_i2c
        esp_tinyusb     # for tusb.h (used internally)
)
