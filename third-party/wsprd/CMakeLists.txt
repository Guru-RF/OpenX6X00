add_library(wsprd STATIC fano.c nhash.c tab.c wsprd.c wsprd_utils.c wsprsim_utils.c)
target_include_directories(wsprd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(wsprd PRIVATE -w)
target_link_libraries(wsprd PUBLIC fftw3f m)
