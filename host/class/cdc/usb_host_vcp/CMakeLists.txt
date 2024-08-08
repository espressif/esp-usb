idf_component_register(SRCS "usb_host_vcp.cpp"
                    INCLUDE_DIRS "include")

set_target_properties(${COMPONENT_LIB} PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)

if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS_EQUAL 13.2)  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=116070
    target_compile_options(${COMPONENT_LIB} PRIVATE -fconcepts)
endif()
