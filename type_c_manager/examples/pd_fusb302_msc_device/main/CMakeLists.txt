idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_partition
        wear_levelling
        fatfs
        type_c              # for esp_typec_pd.h
        type_c_manager      # for esp_typec_manager.h / API
        esp_tinyusb         # TinyUSB integration component
)
