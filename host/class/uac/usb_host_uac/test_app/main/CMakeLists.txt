include($ENV{IDF_PATH}/tools/cmake/version.cmake)

idf_component_register(SRC_DIRS .
                       INCLUDE_DIRS .
                       REQUIRES unity usb usb_host_uac
                       EMBED_FILES new_epic.wav
                       WHOLE_ARCHIVE)
