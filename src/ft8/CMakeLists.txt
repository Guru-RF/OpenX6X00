add_library(FT8 STATIC
    qso.cpp worker.c utils.c gfsk.c
    decode_list.c
    cq_scheduler.c
    tx_worker.c
    audio_worker.c
    hound.c
)

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/..")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../qth")

# Nothing in it uses the radio: no control library (it was linked here until the hw split)
