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

idf_component_register(SRCS "uac_descriptors.c" "uac_host.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_ringbuf
                       REQUIRES "${requires}"
                       )

# package_manager.cmake (component version defines) is provided by the IDF Component Manager.
# With the Component Manager disabled, parse the version from idf_component.yml and
# define the macros manually.
if(cm_enabled)
    include(package_manager)
    cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
else()
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml" ver_line REGEX "^version:")
    string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" ver "${ver_line}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        USB_HOST_UAC_VER_MAJOR=${CMAKE_MATCH_1}
        USB_HOST_UAC_VER_MINOR=${CMAKE_MATCH_2}
        USB_HOST_UAC_VER_PATCH=${CMAKE_MATCH_3}
    )
endif()
