FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/main/*.*)

idf_component_register(SRCS ${app_sources}
                        EMBED_FILES "../files/Image1.jpg"
                        EMBED_FILES "../files/Image2.jpg")
