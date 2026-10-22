idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       REQUIRES unity esp_timer usb usb_host_cdc_acm esp_tinyusb
                       WHOLE_ARCHIVE)

# So we have access to private_include:
target_include_directories(${COMPONENT_LIB} PRIVATE "../../")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
