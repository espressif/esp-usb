idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       REQUIRES unity
                       WHOLE_ARCHIVE)

# So we have access to private_include:
target_include_directories(${COMPONENT_LIB} PRIVATE "../../")
