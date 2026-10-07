# Cross-compile Windows PEs from macOS/Linux.
#
#   cmake -B build/x64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.cmake \
#         -DDXCTS_ARCH=x64
#
# Picks the first usable toolchain: llvm-mingw (covers x86/x64/arm64), then
# mingw-w64 (x86/x64 only), then `zig cc`.  Building with MSVC on Windows needs no
# toolchain file - configure normally and CMake finds cl.

set(CMAKE_SYSTEM_NAME Windows)

if(NOT DXCTS_ARCH)
    set(DXCTS_ARCH x64)
endif()

set(_triples_x64   x86_64-w64-mingw32)
set(_triples_x86   i686-w64-mingw32)
set(_triples_arm64 aarch64-w64-mingw32)

set(_processor_x64   AMD64)
set(_processor_x86   x86)
set(_processor_arm64 ARM64)
set(CMAKE_SYSTEM_PROCESSOR ${_processor_${DXCTS_ARCH}})

set(_triple ${_triples_${DXCTS_ARCH}})
if(NOT _triple)
    message(FATAL_ERROR "DXCTS_ARCH must be one of x86, x64, arm64 (got '${DXCTS_ARCH}')")
endif()

find_program(_clang  NAMES ${_triple}-clang)
find_program(_gcc    NAMES ${_triple}-gcc)
find_program(_zig    NAMES zig)

# C only: the project has no C++ or resource sources, so no compiler is selected for
# either and CMake never looks for one.
if(_clang)                                  # llvm-mingw
    set(CMAKE_C_COMPILER ${_clang})
elseif(_gcc)                                # mingw-w64
    set(CMAKE_C_COMPILER ${_gcc})
elseif(_zig)
    # zig bundles lld and the mingw-w64 headers, so it is the only option here that
    # covers arm64 without a separate install.  Wrappers are needed because CMake
    # invokes the compiler as a single executable.
    set(_wrap ${CMAKE_CURRENT_LIST_DIR}/zig)
    set(CMAKE_C_COMPILER ${_wrap}/zig-cc-${DXCTS_ARCH})
    set(CMAKE_AR         ${_wrap}/zig-ar)
    set(CMAKE_RANLIB     ${_wrap}/zig-ranlib)
else()
    message(FATAL_ERROR
        "No Windows cross-compiler for ${DXCTS_ARCH}.\n"
        "  llvm-mingw (all arches): https://github.com/mstorsjo/llvm-mingw/releases\n"
        "  mingw-w64  (x86/x64):    brew install mingw-w64\n"
        "  zig        (all arches): brew install zig")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
