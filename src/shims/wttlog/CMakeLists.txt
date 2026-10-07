# The export list is generated from the kit; the x86 mangling differs from x64/arm64.
dxcts_shim(shim_wttlog
    OUTPUT_NAME WTTLog
    SOURCES wttlog.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/wttlog-${DXCTS_ARCH}.def)
