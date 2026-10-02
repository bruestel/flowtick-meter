cmake_minimum_required(VERSION 3.16.0)

# The version in the app descriptor, worked out here rather than left to
# ESP-IDF's own `git describe --dirty`: the build touches dependencies.lock
# without changing a byte, and a stat comparison then reports the tree as
# modified. A release from a clean checkout stamped itself "-dirty" that way.
# Refreshing the index first compares contents instead of timestamps.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/../.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} update-index -q --refresh
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE FW_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(FW_VERSION)
        set(PROJECT_VER "${FW_VERSION}")
        message(STATUS "Firmware version: ${PROJECT_VER}")
    endif()
endif()
# Without git ESP-IDF takes version.txt (bumped by hand), and only without
# that does it stamp "1".

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# This string becomes project_name in the app descriptor, and the OTA handler
# rejects an image whose project_name it does not know -- the guard against
# flashing somebody else's firmware. Changing it means every deployed device
# refuses the next update and needs a cable, unless an update that also accepts
# the new name goes first (see same_project() in app_web; that is how
# "water-meter" became "flowtick-meter").
project(flowtick-meter)
