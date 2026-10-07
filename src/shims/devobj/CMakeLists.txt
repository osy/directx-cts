# Pure forwarders: the .def is the whole implementation.
dxcts_shim(shim_devobj
    OUTPUT_NAME DEVOBJ
    SOURCES ../../common/empty.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/devobj.def)
