# Mercury: the second Winlink modem for Pat (experimental), as its daemon build: the engine without Mercury's own
# audio, PTT and websocket server, for a program of ours that links it in and owns the radio (host/mercury_host.h).
# The sources of upstream's `make libmercury_host.a MERCURY_HOST=daemon`, with the FreeDV library, the iniparser
# and the hermes_shm rings it leaves to the host; see UPSTREAM.md

# modem/freedv/Makefile's libfreedvdata.a
set(MERCURY_FREEDV
    freedv_api ofdm freedv_fsk cohpsk fsk kiss_fft freedv_1600 fdmdv codec2_fft codec2 pack kiss_fftr newamp1
    quantise lsp lpc mbest phase sine varicode freedv_700 freedv_vhf_framing fmfsk filter freedv_data_channel
    ldpc_codes mpdecode_core interldpc golay23 linreg nlp codebooknewamp1 codebooknewamp1_energy codebooknewamp2
    codebooknewamp2_energy postfilter interp codebook codebookd codebookge codebookjmv ofdm_mode HRA_112_112
    HRA_56_56 HRAb_396_504 H_256_768_22 H_256_512_4 HRAa_1536_512 H_128_256_5 H_2064_516_sparse H_4096_8192_3d
    H_16200_9720 H_1024_2048_4f H_212_158 phi0 gp_interleaver modem_stats)
list(TRANSFORM MERCURY_FREEDV PREPEND modem/freedv/)

# The Makefile's MERCURY_CORE_OBJS for the daemon, then HOST_AUDIO_OBJS, HOST_API_OBJS, BCAST_FILE_OBJS (the
# broadcast carousel with RaptorQ) and the bridge (mercury_init, mercury_shutdown)
set(MERCURY_SOURCES
    common/cfg_utils
    datalink_arq/arq datalink_arq/arq_trace datalink_arq/arq_tnc datalink_arq/arith datalink_arq/arq_channels
    datalink_arq/arq_fsm datalink_arq/arq_protocol datalink_arq/arq_timing datalink_arq/arq_modem
    datalink_arq/carousel datalink_arq/rs_erasure
    datalink_broadcast/broadcast datalink_broadcast/kiss
    modem/modem modem/modem_freedv modem/modem_mfsk modem/mfsk/mfsk modem/mfsk/mfsk_ofdm modem/mfsk/mfsk_sync
    modem/mfsk/mfsk_ldpc modem/mfsk/mfsk_ldpc_1_16 modem/mfsk/mfsk_ldpc_2_16 modem/mfsk/mfsk_ldpc_3_16
    modem/mfsk/mfsk_ldpc_5_16 modem/mfsk/mfsk_ldpc_8_16 modem/framer modem/channel_busy modem/monitor_frame
    common/os_interop common/crc6 common/hermes_log common/virtual_clock common/chan common/queue
    common/mercury_engine common/mercury_cli common/mercury_modes common/message_store
    data_interfaces/tcp_interfaces data_interfaces/net
    gui_interface/ui_communication gui_interface/ui_status gui_interface/ui_devices gui_interface/ui_history
    gui_interface/websocket/ws_json
    host/ui_host host/radio_host host/audio_host host/api_host
    datalink_broadcast/bcast_file datalink_broadcast/bcast_carousel datalink_broadcast/bcast_rx_host
    datalink_broadcast/raptorq/lib/chooser datalink_broadcast/raptorq/lib/io datalink_broadcast/raptorq/lib/nanorq
    datalink_broadcast/raptorq/lib/nanorq_core datalink_broadcast/raptorq/lib/ops datalink_broadcast/raptorq/lib/params
    datalink_broadcast/raptorq/lib/partition datalink_broadcast/raptorq/lib/precode datalink_broadcast/raptorq/lib/rand
    datalink_broadcast/raptorq/lib/sopi datalink_broadcast/raptorq/lib/tuple datalink_broadcast/raptorq/lib/uvec
    datalink_broadcast/raptorq/deps/obl/oblas_common datalink_broadcast/raptorq/deps/obl/oblas_lite
    gui_interface/fyne-ui/engine/mercury_bridge
    # What the daemon build leaves to the host: the vendored iniparser (the X6200's sysroot has none) and the
    # hermes_shm rings host/audio_host.c is compiled against
    common/iniparser/iniparser common/iniparser/dictionary
    common/ring_buffer_posix common/shm_posix
    ${MERCURY_FREEDV})
list(TRANSFORM MERCURY_SOURCES APPEND .c)

add_library(mercury_daemon STATIC ${MERCURY_SOURCES})
# gnu11, as upstream's config.mk
set_target_properties(mercury_daemon PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
# As upstream builds it: -O2, and its asserts kept (a release build's NDEBUG would drop them)
target_compile_options(mercury_daemon PRIVATE -O2 -pthread -UNDEBUG)
target_compile_definitions(mercury_daemon PRIVATE _GNU_SOURCE GIT_HASH="edca701b")
# The include paths of upstream's sub-makes, in one list (no two of its headers share a name)
target_include_directories(mercury_daemon BEFORE PRIVATE
    . modem/freedv modem/mfsk modem datalink_broadcast data_interfaces datalink_arq audioio common
    common/iniparser gui_interface gui_interface/websocket radio_io host
    datalink_broadcast/raptorq/include datalink_broadcast/raptorq/deps)
# The host program sees host/mercury_host.h only
target_include_directories(mercury_daemon INTERFACE host)
target_link_libraries(mercury_daemon PUBLIC m pthread rt)
# 32-bit ARM has no 64-bit atomic instructions: GCC calls libatomic for Mercury's 64-bit _Atomic counters
# (upstream's config.mk links it there too)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_libraries(mercury_daemon PUBLIC atomic)
endif()
