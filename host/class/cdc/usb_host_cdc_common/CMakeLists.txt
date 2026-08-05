idf_component_register(SRCS
                        "cdc_host_common.c"
                        "cdc_host_descriptor_parsing.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES esp_ringbuf
                       )
