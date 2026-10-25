add_executable(dxcts main.c catalog.c listing.c report.c results.c spawn.c
               ../common/json.c)
set_target_properties(dxcts PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DXCTS_OUT})
if(NOT MSVC)
    target_link_options(dxcts PRIVATE -static-libgcc)
endif()
