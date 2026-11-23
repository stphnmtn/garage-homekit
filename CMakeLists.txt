cmake_minimum_required(VERSION 3.16)

# Integer, strictly increasing: the OTA receiver refuses older images.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/VERSION PROJECT_VER LIMIT_COUNT 1)
if(PROJECT_VER_OVERRIDE)  # tests only (tools/qemu-test-inner.sh)
    set(PROJECT_VER ${PROJECT_VER_OVERRIDE})
endif()

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(garage-homekit)
