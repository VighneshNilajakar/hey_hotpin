cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-waker)

# The Edge Impulse export shipped its own top-level CMakeLists.txt targeting
# Zephyr. It is preserved here for reference:
#
#   cmake_minimum_required(VERSION 3.13.1)
#   if(NOT TARGET app)
#        message(FATAL_ERROR "Please create a target named 'app' ...")
#   endif()
#   include(edge-impulse-sdk/cmake/utils.cmake)
#   add_subdirectory(edge-impulse-sdk/cmake/zephyr)
#   RECURSIVE_FIND_FILE_APPEND(MODEL_SOURCE "tflite-model" "*.cpp")
#   target_include_directories(app PRIVATE .)
#   target_sources(app PRIVATE ${MODEL_SOURCE})
