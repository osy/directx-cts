dxcts_shim(shim_rpcrt4
    OUTPUT_NAME dxrpc4
    SOURCES rpcrt4.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/rpcrt4.def
    LIBS rpcrt4)
