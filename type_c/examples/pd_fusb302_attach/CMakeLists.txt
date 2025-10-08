cmake_minimum_required(VERSION 3.16)

# Tell IDF where to find the local "type_c" component (two levels up).
# This lets the example depend on your in-tree component without copying it.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pd_fusb302_attach)
