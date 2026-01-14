idf_component_register(SRC_DIRS . parsing streaming opening
                        REQUIRES cmock
                        INCLUDE_DIRS . parsing streaming opening
                        PRIV_INCLUDE_DIRS "../../private_include"
                        WHOLE_ARCHIVE)

# We do not use main() from esp-idf as it discards argc and argv arguments
# We wrap main so argc and argv are correctly passed to Catch2
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=main")
