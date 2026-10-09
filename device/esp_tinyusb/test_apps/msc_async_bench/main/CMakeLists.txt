set(priv_requires esp_blockdev)
if(CONFIG_BENCH_STORAGE_SDCARD)
    list(APPEND priv_requires esp_driver_sdmmc sdmmc)
endif()

idf_component_register(SRCS "bench_main.c"
                       PRIV_REQUIRES ${priv_requires})
