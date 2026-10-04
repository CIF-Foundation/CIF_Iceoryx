cmake_minimum_required(VERSION 3.22)

add_library(iox2_cif SHARED src/iox2_cif.c)
add_library(iox2_cif::iox2_cif ALIAS iox2_cif)

target_include_directories(iox2_cif
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(iox2_cif PRIVATE IOX2_CIF_EXPORTS)
target_compile_features(iox2_cif PRIVATE c_std_11)

# Single DLL: iceoryx2 C FFI is statically linked into iox2_cif.dll.
target_link_libraries(iox2_cif PRIVATE iceoryx2-c::static-lib)

if(TARGET iceoryx2-ffi-c-build-step)
    add_dependencies(iox2_cif iceoryx2-ffi-c-build-step)
endif()

if(MSVC)
    target_compile_options(iox2_cif PRIVATE /W3)
    set_target_properties(iox2_cif PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
else()
    target_compile_definitions(iox2_cif PRIVATE _GNU_SOURCE)
    target_compile_options(iox2_cif PRIVATE -Wall -fPIC)
    set_target_properties(iox2_cif PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(iox2_cif PRIVATE pthread m ${CMAKE_DL_LIBS})
endif()

set_target_properties(iox2_cif PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    OUTPUT_NAME iox2_cif
)

add_executable(iox2_cif_smoke tests/smoke.c)
target_link_libraries(iox2_cif_smoke PRIVATE iox2_cif)
set_target_properties(iox2_cif_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(UNIX)
    set_target_properties(iox2_cif_smoke PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()
