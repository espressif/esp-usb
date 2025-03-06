idf_component_register(SRC_DIRS .
                        REQUIRES cmock usb
                        INCLUDE_DIRS .
                        WHOLE_ARCHIVE)

#target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain) # We don't mock FreeRTOS for this test
