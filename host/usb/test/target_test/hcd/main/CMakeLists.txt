# In order for the cases defined by `TEST_CASE` to be linked into the final elf,
# the component can be registered as WHOLE_ARCHIVE
if(${IDF_VERSION_MAJOR} LESS 6)
    set(usb_hal_requires hal)
else()
    # For IDF 6.x and later, the USB DWC HAL driver is moved to esp_hal_usb
    set(usb_hal_requires esp_hal_usb)
endif()

idf_component_register(SRC_DIRS "."
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES usb unity common esp_mm ${usb_hal_requires}
                       WHOLE_ARCHIVE)
