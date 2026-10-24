# NOTE: This kind of mocking currently works on Linux targets only.
#
# Overrides the real `usb_host_cdc_common` component with CMock-generated stubs
# for the public API (`cdc_host_common.h`), so consumers of this component
# (typically `usb_host_cdc_acm`) can be exercised in isolation.
#
# The mock is selected by `override_path` in the consumer's manifest (see
# `parsing_tests/main/idf_component.yml`); the component manager registers this
# path *after* EXTRA_COMPONENT_DIRS, which is required for the override to win.

message(STATUS "building CDC Host Common MOCKS")

get_filename_component(original_common_dir "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_mock(INCLUDE_DIRS "${original_common_dir}/include"
                                "${original_common_dir}/include/usb"
                                "${original_common_dir}/private_include"
                   MOCK_HEADER_FILES "${original_common_dir}/include/usb/cdc_host_common.h"
                   REQUIRES usb freertos)
