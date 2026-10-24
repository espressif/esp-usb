set(requires "")
if((${IDF_VERSION_MAJOR} LESS 6) OR ("${IDF_TARGET}" STREQUAL "linux"))
    # IDF 5.x and Linux host tests need an explicit usb component requirement.
    list(APPEND requires usb)
endif()

idf_component_register(SRCS
                        "cdc_host_common.c"
                        "cdc_host_descriptor_parsing.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES esp_ringbuf
                       REQUIRES "${requires}"
                       )
