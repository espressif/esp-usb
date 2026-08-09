idf_component_register(SRCS "test_app_main.c" "test_runtime_composite.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../../include_private"
                       PRIV_REQUIRES unity esp_tinyusb esp_timer)
