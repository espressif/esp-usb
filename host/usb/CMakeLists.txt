idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    return() # This component is not supported by the POSIX/Linux simulator
endif()

set(srcs)
set(include)
set(priv_includes)
# As CONFIG_SOC_USB_OTG_SUPPORTED comes from Kconfig, it is not evaluated yet
# when components are being registered.
# Thus, always add the (private) requirements, regardless of Kconfig
set(priv_requires esp_mm)

# Explicitly add psram component for esp32p4, as the USB-DWC internal DMA can access PSRAM on esp32p4
if(${target} STREQUAL "esp32p4")
    list(APPEND priv_requires esp_psram)
endif()

if(CONFIG_SOC_USB_OTG_SUPPORTED)
    list(APPEND srcs "src/hcd_dwc.c"
                     "src/enum.c"
                     "src/hub.c"
                     "src/usb_helpers.c"
                     "src/usb_host.c"
                     "src/usb_private.c"
                     "src/usbh.c"
                     )
    list(APPEND include "include")
    list(APPEND priv_includes "private_include")
endif()

if(CONFIG_USB_HOST_HUBS_SUPPORTED)
    list(APPEND srcs "src/ext_hub.c"
                     "src/ext_port.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include}
                       PRIV_INCLUDE_DIRS ${priv_includes}
                       PRIV_REQUIRES ${priv_requires}
                       )

if(CONFIG_COMPILER_STATIC_ANALYZER AND CMAKE_C_COMPILER_ID STREQUAL "GNU") # TODO GCC-366 (segfault)
    set_property(SOURCE usb_host.c PROPERTY COMPILE_FLAGS -fno-analyzer)
endif()
