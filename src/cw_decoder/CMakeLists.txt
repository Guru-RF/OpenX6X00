# CW receiver: C++17 with liquid-dsp for the FFT, NCO, filters and resampler.
# Used by the app (src/cw.cpp, the Morse table also by the keyer) and by tests/cw_decoder.
add_library(CW_DECODER STATIC
    morse_code.c
    morse_text.cpp
    tone_finder.cpp
    tone_lock.cpp
    key_detector.cpp
    element_timer.cpp
    receiver.cpp
)

target_compile_features(CW_DECODER PUBLIC cxx_std_17)
target_include_directories(CW_DECODER PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(CW_DECODER PUBLIC liquid)
