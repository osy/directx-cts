dxcts_shim(shim_devobj
    OUTPUT_NAME DEVOBJ
    SOURCES devobj.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/devobj.def
    LIBS setupapi)
