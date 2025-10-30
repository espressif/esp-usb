# 1. USB component dependency in IDF < 6.0 is used only to access USB PHY
# 2. USB PHY was removed from the usb component in IDF 6.0 to esp_hw_support
set(priv_requires "")
if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires usb)
endif()

idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       PRIV_INCLUDE_DIRS "../../../include_private"
                       PRIV_REQUIRES ${priv_requires}
                       REQUIRES unity
                       WHOLE_ARCHIVE)
