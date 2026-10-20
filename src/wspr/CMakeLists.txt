add_library(WSPR STATIC wspr.c wsprnet.c)
target_include_directories(WSPR PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WSPR PUBLIC wsprd liquid pthread)
