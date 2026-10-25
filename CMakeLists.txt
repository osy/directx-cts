cmake_minimum_required(VERSION 3.20)
project(directx-cts C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR
        "directx-cts builds Windows binaries. Cross-compile with:\n"
        "  cmake -B build/x64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.cmake "
        "-DDXCTS_ARCH=x64")
endif()

if(NOT DXCTS_ARCH)
    set(DXCTS_ARCH x64)
endif()

# DXCTS_ARCH picks the WTTLog proxy's .def, whose C++ manglings and calling convention
# differ per architecture, so it has to be the architecture actually being built.  The
# toolchain file ties the two together; a native MSVC build has nothing tying them, and
# the mismatch would link cleanly and produce a proxy exporting names no importer asks
# for - so it is checked here rather than discovered at run time.
if(DXCTS_ARCH STREQUAL "x86")
    set(_expect_pointer 4)
else()
    set(_expect_pointer 8)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL _expect_pointer)
    message(FATAL_ERROR
        "DXCTS_ARCH=${DXCTS_ARCH} wants ${_expect_pointer}-byte pointers but the "
        "compiler produces ${CMAKE_SIZEOF_VOID_P}-byte ones. Pass the DXCTS_ARCH that "
        "matches the target you configured.")
endif()

set(DXCTS_OUT ${CMAKE_BINARY_DIR}/bin)

# Shim DLLs replace Microsoft-named modules, so the output name is fixed and the
# import library is of no use to us.  The .def is a shim's whole export list:
# __declspec(dllexport) would add x86's decorated names too, which the linker numbers
# into free ordinals - and some importers ask for a stock ordinal.
function(dxcts_shim name)
    cmake_parse_arguments(A "" "OUTPUT_NAME;DEF" "SOURCES;LIBS" ${ARGN})
    add_library(${name} SHARED ${A_SOURCES})
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${A_OUTPUT_NAME}"
        RUNTIME_OUTPUT_DIRECTORY ${DXCTS_OUT}
        LIBRARY_OUTPUT_DIRECTORY ${DXCTS_OUT})
    if(A_LIBS)
        target_link_libraries(${name} PRIVATE ${A_LIBS})
    endif()
    if(A_DEF)
        if(MSVC)
            target_link_options(${name} PRIVATE "/DEF:${A_DEF}")
        else()
            target_sources(${name} PRIVATE ${A_DEF})
        endif()
    endif()
    if(NOT MSVC)
        # The .def already fixes the export set, so no --exclude-all-symbols (which
        # zig's lld rejects). -static-libgcc keeps a drop-in DLL free of a mingw
        # runtime dependency.
        target_link_options(${name} PRIVATE -static-libgcc)
    endif()
endfunction()

add_subdirectory(src/shims/backend_d3dmetal)
add_subdirectory(src/shims/crtprivate)
add_subdirectory(src/shims/d3d11)
add_subdirectory(src/shims/d3d12)
add_subdirectory(src/shims/d3d9)
add_subdirectory(src/shims/d3dkmt)
add_subdirectory(src/shims/dcomp)
add_subdirectory(src/shims/devobj)
add_subdirectory(src/shims/dxgi)
add_subdirectory(src/shims/gdi32)
add_subdirectory(src/shims/msvcrt)
add_subdirectory(src/shims/oleaut32)
add_subdirectory(src/shims/powersetting)
add_subdirectory(src/shims/powrprof)
add_subdirectory(src/shims/rpcrt4)
add_subdirectory(src/shims/wttlog)
add_subdirectory(src/runner)
