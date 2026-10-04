cmake_minimum_required(VERSION 3.22)
project(iox2_cif C)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/iceoryx2/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/iceoryx2 is missing. Clone it first:\n"
        "  git clone --depth 1 https://github.com/eclipse-iceoryx/iceoryx2.git third_party/iceoryx2")
endif()

set(BUILD_CXX OFF CACHE BOOL "Build iceoryx2 C++ bindings" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build iceoryx2 examples" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Build iceoryx2 tests" FORCE)

add_subdirectory(third_party/iceoryx2 EXCLUDE_FROM_ALL)
add_subdirectory(native)
