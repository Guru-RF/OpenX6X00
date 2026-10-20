# JS8: receiver, transmitter and the dialog's helpers, built on js8core.
find_package(Threads REQUIRED) # the receiver and transmitter run their own threads

add_library(JS8 STATIC rate_conv.cpp submodes.cpp frame_text.cpp frame_joiner.cpp msg_kind.cpp directed_msg.cpp rx_engine.cpp
    wav_file.cpp tx_engine.cpp js8_transmit.cpp command_words.cpp heard_list.cpp auto_answer.cpp qso_book.cpp msg_store.cpp alert_rules.cpp macros.cpp
    js8_logic.cpp js8_receive.cpp)

# js8core is C++20. Linking it PRIVATE keeps that inside this library: the
# GUI only includes the C headers and builds with its own C++ standard.
target_link_libraries(JS8 PRIVATE js8core)
target_link_libraries(JS8 PUBLIC Threads::Threads)
target_include_directories(JS8 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Synthetic JS8 signals for the tests and tools/js8_wavgen. The firmware doesn't link it.
add_library(JS8TestSignal STATIC
    test_signal.cpp)
target_link_libraries(JS8TestSignal PRIVATE JS8 js8core)
target_include_directories(JS8TestSignal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
