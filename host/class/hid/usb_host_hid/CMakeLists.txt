# 1. IDF version >= 6.0 does not have usb component: usb from IDF component manager will be used
# 2. If IDF component manager is disabled, usb from IDF component manager cannot be used either
# 3. For linux target, we can't use IDF component manager to get usb component, we need to add it 'the old way'
#    with EXTRA_COMPONENT_DIRS because mocking of managed components is not supported yet.
#    This is acceptable workaround for testing.
set(requires "")
idf_build_get_property(cm_enabled IDF_COMPONENT_MANAGER)
if((${IDF_VERSION_MAJOR} LESS 6) OR (NOT cm_enabled) OR ("${IDF_TARGET}" STREQUAL "linux"))
    list(APPEND requires usb)
endif()

idf_component_register(SRCS "hid_host.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "${requires}"
                       )
