idf_component_register(SRC_DIRS .
                        REQUIRES cmock
                        INCLUDE_DIRS "../../" .
                        PRIV_INCLUDE_DIRS "../../../private_include"
                        WHOLE_ARCHIVE)

# We do not use main() from esp-idf as it discards argc and argv arguments
# We wrap main so argc and argv are correctly passed to Catch2
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=main")

# Set public symbol REMOTE_WAKE_HAL_SUPPORTED, which is normally set in usb component cmake
# and depends on remote wakeup feature presence in USB HAL/LL
set(REMOTE_WAKE_HAL_SUPPORTED ON)
target_compile_definitions(${COMPONENT_LIB} PUBLIC REMOTE_WAKE_HAL_SUPPORTED)
