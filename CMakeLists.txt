cmake_minimum_required(VERSION 3.16.0)
# The epdiy library joins the build only for the epdiy board targets, which set
# -D TESSERAE_EPDIY=1 in board_build.cmake_extra_args; it is ESP32/S3-only and
# every other target neither fetches nor links it.
if(TESSERAE_EPDIY)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components_opt/epdiy_dep")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tesserae-device-firmware)
