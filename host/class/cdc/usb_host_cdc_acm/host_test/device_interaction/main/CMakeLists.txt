idf_component_register(SRC_DIRS .
                        REQUIRES cmock
                        PRIV_REQUIRES esp_ringbuf
                        INCLUDE_DIRS "../../" .
                        PRIV_INCLUDE_DIRS "../../../private_include"
                        WHOLE_ARCHIVE)

# Inject the enqueue at the TX idle transition without changing production code.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=vRingbufferGetInfo")
