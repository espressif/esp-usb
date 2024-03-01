include($ENV{IDF_PATH}/tools/cmake/version.cmake)

idf_component_register(SRCS "test_esp_tinyusb.c" "test_bvalid_sig.c" "test_app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES unity esp_tinyusb
                       WHOLE_ARCHIVE)