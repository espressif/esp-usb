idf_component_register(SRCS
                        "uvc_host.c"
                        "uvc_descriptor_parsing.c"
                        "uvc_descriptor_printing.c"
                        "uvc_frame.c"
                        "uvc_control.c"
                        "uvc_isoc.c"
                        "uvc_bulk.c"
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS private_include include/esp_private
                       PRIV_REQUIRES heap
                       REQUIRES usb
                       )
