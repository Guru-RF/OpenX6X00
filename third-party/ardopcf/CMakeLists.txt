# ardopcf: the ARDOP modem Pat uses for Winlink, its audio and PTT through the GUI's socket
# (src/linux/SockSound.c). The Linux sources of upstream's Makefile; see UPSTREAM.md

# The WebGui's page and script are compiled in as C strings
set(ARDOPCF_WEBGUI "")
foreach(f webgui.html webgui.js)
    string(REPLACE "." "_" var ${f})
    set(out ${CMAKE_CURRENT_BINARY_DIR}/gen-${f}.c)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/webgui/${f} -DOUT=${out}
                -DVAR=${var} -P ${CMAKE_CURRENT_SOURCE_DIR}/txt2c.cmake
        DEPENDS webgui/${f} txt2c.cmake
        VERBATIM)
    list(APPEND ARDOPCF_WEBGUI ${out})
endforeach()

add_executable(ardopcf
    lib/rawhid/rawhid.c
    lib/rockliff/rrs.c
    lib/ws_server/ws_server.c
    src/common/ARDOPC.c
    src/common/ARDOPCommon.c
    src/common/ardopSampleArrays.c
    src/common/ARQ.c
    src/common/BusyDetect.c
    src/common/FEC.c
    src/common/FFT.c
    src/common/HostInterface.c
    src/common/Locator.c
    src/common/log_file.c
    src/common/log.c
    src/common/Modulate.c
    src/common/Packed6.c
    src/common/RXO.c
    src/common/sdft.c
    src/common/SoundInput.c
    src/common/StationId.c
    src/common/TCPHostInterface.c
    src/common/txframe.c
    src/common/wav.c
    src/common/Webgui.c
    src/common/noise.c
    src/linux/ALSASound.c
    src/linux/LinSerial.c
    src/linux/SockSound.c
    src/common/ardopcf.c
    ${ARDOPCF_WEBGUI})
# Written for C before C23: an empty () declares a function with any arguments
set_target_properties(ardopcf PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(ardopcf BEFORE PRIVATE src lib)
# Upstream's own build has no optimisation at all
target_compile_options(ardopcf PRIVATE -O2)
target_link_libraries(ardopcf PRIVATE asound m pthread rt)
install(TARGETS ardopcf DESTINATION bin)
