dxcts_shim(shim_d3d11
    OUTPUT_NAME dxc11
    SOURCES d3d11.c ../../common/comwrap.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/d3d11.def)
