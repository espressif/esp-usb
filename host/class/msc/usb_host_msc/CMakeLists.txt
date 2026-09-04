# 1. IDF version >= 6.0 does not have usb component: usb from IDF component manager will be used
# 2. For linux target, we can't use IDF component manager to get usb component, we need to add it 'the old way'
#    with EXTRA_COMPONENT_DIRS because mocking of managed components is not supported yet.
#    This is acceptable workaround for testing.
set(requires "fatfs")
if((${IDF_VERSION_MAJOR} LESS 6) OR ("${IDF_TARGET}" STREQUAL "linux"))
    list(APPEND requires usb)
endif()

set(priv_requires heap)
set(sources src/msc_scsi_bot.c
            src/msc_host.c
            src/msc_host_vfs.c)
# 6.1+: MSC exposes SCSI as an esp_blockdev handle (msc_host_get_blockdev).
# FatFS diskio lives in IDF (diskio_bdl.c). Pre-6.1: this component registers
# SCSI callbacks itself (diskio_usb.c).
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "6.1")
    list(APPEND requires esp_blockdev)
    list(APPEND sources src/msc_bdl.c)
else()
    list(APPEND sources src/diskio_usb.c)
endif()

idf_component_register(SRCS ${sources}
                       INCLUDE_DIRS include include/usb # 'include/usb' is here for backwards compatibility
                       PRIV_INCLUDE_DIRS private_include include/esp_private
                       REQUIRES ${requires}
                       PRIV_REQUIRES ${priv_requires}
                       )
