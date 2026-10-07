# Pure forwarders: the .def is the whole implementation.
dxcts_shim(shim_crtprivate
    OUTPUT_NAME dxcts_crt_private_compat_shim
    SOURCES ../../common/empty.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/crtprivate.def)
