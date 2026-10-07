dxcts_shim(shim_d3d12
    OUTPUT_NAME dxc12
    SOURCES d3d12.c ../../common/comwrap.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/d3d12.def)
