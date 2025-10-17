idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_driver_gpio driver
                       REQUIRES type_c)
