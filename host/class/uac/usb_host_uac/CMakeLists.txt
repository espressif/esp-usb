idf_component_register( SRCS "uac_descriptors.c" "uac_host.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES usb esp_ringbuf)

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
