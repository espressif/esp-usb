# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Needed for CI setup: When IDF Component Manager is disabled, idf_component.yml manifests (incl. override_path)
# are not processed. Provide the dependencies via EXTRA_COMPONENT_DIRS instead.
if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
    list(APPEND EXTRA_COMPONENT_DIRS
         ".."                 # The class driver under test
         "../../../../usb"    # USB Host library
    )
endif()

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_app_usb_host_uvc)
