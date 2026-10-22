dxcts_shim(shim_msvcrt
    OUTPUT_NAME dxmsvc
    SOURCES msvcrt.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/msvcrt.def)
