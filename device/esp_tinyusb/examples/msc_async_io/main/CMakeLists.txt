set(priv_requires esp_blockdev esp_partition wear_levelling esp_timer)
if(CONFIG_EXAMPLE_STORAGE_SDCARD)
    list(APPEND priv_requires esp_driver_sdmmc sdmmc)
endif()

idf_component_register(SRCS "msc_async_io_main.c"
                       PRIV_REQUIRES ${priv_requires})
