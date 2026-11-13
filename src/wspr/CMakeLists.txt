add_library(WSPR STATIC wspr.c wsprnet.c wspr_hop.c)
target_include_directories(WSPR PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# cfg: wspr_hop.c finds the band of a frequency in cfg/ham_bands.c
target_link_libraries(WSPR PUBLIC wsprd liquid pthread cfg)
