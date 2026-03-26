idf_component_register(SRCS "dev_hid.c"
                            "dev_isoc.c"
                            "dev_msc.c"
                            "mock_msc.c"
                            "phy_common.c"
                            "hcd_common.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb unity
                       PRIV_REQUIRES esp_mm
                       )

# Add usb component's private includes (hcd.h) via target_include_directories so component_validation in cmake
# does not flag a foreign path in idf_component_register(PRIV_INCLUDE_DIRS ...).
idf_component_get_property(usb_component_dir usb COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PUBLIC "${usb_component_dir}/private_include")
