set(srcs)
set(include)
set(private_include)
# As CONFIG_USB_OTG_SUPPORTED comes from Kconfig, it is not evaluated yet
# when components are being registered.
set(require usb)

if(CONFIG_USB_OTG_SUPPORTED)
    list(APPEND srcs "uvc_host.c" "uvc_descriptors.c" "uvc_frame.c" "uvc_control.c")
    list(APPEND include "include")
    list(APPEND private_include "private_include" "include/esp_private")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include}
                       PRIV_INCLUDE_DIRS ${private_include}
                       REQUIRES ${require}
                       )
