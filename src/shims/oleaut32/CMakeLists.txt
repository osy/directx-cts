# Pure forwarders: the .def is the whole implementation.
dxcts_shim(shim_oleaut32
    OUTPUT_NAME dxctsole
    SOURCES ../../common/empty.c
    DEF ${CMAKE_CURRENT_SOURCE_DIR}/oleaut32.def)
