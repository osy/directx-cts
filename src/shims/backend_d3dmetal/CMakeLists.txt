dxcts_shim(backend_d3dmetal
    OUTPUT_NAME dxcts_backend_d3dmetal
    SOURCES backend.c formats.c d3d11.c d3d12.c ../../common/comwrap.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/backend_d3dmetal.def
    LIBS dxguid uuid)
