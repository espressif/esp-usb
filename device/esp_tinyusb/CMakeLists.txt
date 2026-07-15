if(IDF_BUILD_V2)
    include(CMakeLists_v2.txt)
    return()
endif()

idf_build_get_property(target IDF_TARGET)
set(IDF_VERSION "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

# required by rndis_reports.c: #include "netif/ethernet.h"
set(priv_req esp_netif)
set(priv_includes
    "include_private"
    "tinyusb/src/device"
)

set(srcs
    # esp_tinyusb core
    "descriptors_control.c"
    "tinyusb.c"
    "usb_descriptors.c"
    "tinyusb_task.c"

    # TinyUSB core
    "tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c"
    "tinyusb/src/portable/synopsys/dwc2/dwc2_common.c"
    "tinyusb/src/common/tusb_fifo.c"
    "tinyusb/src/device/usbd.c"
    "tinyusb/src/tusb.c"

    # TinyUSB classes
    "tinyusb/src/class/mtp/mtp_device.c"
    "tinyusb/src/class/printer/printer_device.c"
    "tinyusb/src/class/audio/audio_device.c"
    "tinyusb/src/class/video/video_device.c"
    "tinyusb/src/class/usbtmc/usbtmc_device.c"
    )

# USB-PHY driver
# For IDF 6.x and later, usb_phy driver is moved from usb to esp_hw_support
if(IDF_VERSION VERSION_LESS "6.0.0")
    list(APPEND priv_req usb)
else()
    list(APPEND priv_req esp_hw_support)
endif()

if(CONFIG_TINYUSB_CDC_ENABLED)
    list(APPEND srcs
        "cdc.c"
        "tinyusb_cdc_acm.c"
        "tinyusb/src/class/cdc/cdc_device.c"
        )
    if(CONFIG_VFS_SUPPORT_IO)
        list(APPEND srcs
            "tinyusb_console.c"
            "vfs_tinyusb.c"
            )
    endif() # CONFIG_VFS_SUPPORT_IO
endif() # CONFIG_TINYUSB_CDC_ENABLED

if(CONFIG_TINYUSB_MSC_ENABLED)
    list(APPEND srcs
        "tinyusb_msc.c"
        "storage_spiflash.c"
        "tinyusb/src/class/msc/msc_device.c"
        )
    if(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
        list(APPEND srcs
            "storage_sdmmc.c"
            )
    endif() # CONFIG_SOC_SDMMC_HOST_SUPPORTED
endif() # CONFIG_TINYUSB_MSC_ENABLED


if(CONFIG_TINYUSB_NET_MODE_NCM)
    list(APPEND srcs
         "tinyusb_net.c"
         "tinyusb/src/class/net/ncm_device.c"
         )
endif() # CONFIG_TINYUSB_NET_MODE_NCM

if(CONFIG_TINYUSB_NET_MODE_ECM_RNDIS)
    list(APPEND srcs
         "tinyusb/src/class/net/ecm_rndis_device.c"
         "tinyusb/lib/networking/rndis_reports.c"
         )
    list(APPEND priv_includes "tinyusb/lib/networking")
endif()

if(CONFIG_TINYUSB_DFU_MODE_DFU)
    list(APPEND srcs
         "tinyusb/src/class/dfu/dfu_device.c"
         )
endif()

if(CONFIG_TINYUSB_DFU_MODE_DFU_RUNTIME)
    list(APPEND srcs
         "tinyusb/src/class/dfu/dfu_rt_device.c"
         )
endif()

if(CONFIG_TINYUSB_HID_COUNT GREATER 0)
    list(APPEND srcs
         "tinyusb/src/class/hid/hid_device.c"
         )
endif()

if(CONFIG_TINYUSB_MIDI_COUNT GREATER 0)
    list(APPEND srcs
         "tinyusb/src/class/midi/midi_device.c"
         "tinyusb/src/class/midi/midi2_device.c"
         )
endif()

if(CONFIG_TINYUSB_BTH_ENABLED)
    list(APPEND srcs
         "tinyusb/src/class/bth/bth_device.c"
         )
endif()

if(CONFIG_TINYUSB_VENDOR_COUNT GREATER 0)
    list(APPEND srcs
         "tinyusb/src/class/vendor/vendor_device.c"
         )
endif()

# Software VBUS monitoring is needed for chips with HS USB-OTG peripheral
if(${target} STREQUAL "esp32p4" OR ${target} STREQUAL "esp32s31")
    # VBUS monitoring requires GPIO driver
    list(APPEND priv_req esp_driver_gpio)
    list(APPEND srcs "tinyusb_vbus_monitor.c")
endif() # esp32p4/esp32s31

if(${target} STREQUAL "esp32p4")
    list(APPEND priv_req
                esp_mm # required by dcd_dwc2.c: #include "esp_cache.h"
        )
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include" "tinyusb/src"
                       PRIV_INCLUDE_DIRS ${priv_includes}
                       PRIV_REQUIRES ${priv_req}
                       REQUIRES fatfs vfs freertos
                       )

string(TOUPPER "OPT_MCU_${target}" tusb_mcu)
target_compile_options(${COMPONENT_LIB} PUBLIC "-DCFG_TUSB_MCU=${tusb_mcu}")

# when no builtin class driver is enabled, an uint8_t data compared with `BUILTIN_DRIVER_COUNT` will always be false
set_source_files_properties("tinyusb/src/device/usbd.c" PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
