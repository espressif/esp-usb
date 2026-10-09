set(priv_requires esp_blockdev esp_partition wear_levelling esp_timer esp_netif esp_event esp_http_server lwip)
if(CONFIG_EXAMPLE_STORAGE_SDCARD)
    list(APPEND priv_requires esp_driver_sdmmc sdmmc)
endif()

idf_component_register(SRCS "msc_ncm_main.c" "storage.c" "ncm_netif.c" "net_servers.c"
                       PRIV_REQUIRES ${priv_requires})
