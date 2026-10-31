# The GUI program, x6100_gui. Copyright (c) 2026 Giovanni Ricciardi IU1WXY
# The source order below is the link order: keep it when adding files.
add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PUBLIC
    gui_main.c screen_main.c walkie.c gui_styles.c mono.c wf_palettes.c
    spectrum_view.c radio.c dsp.cpp util.cpp
    waterfall_view.c knob_device.c key_input.c input_caps.c mfk_device.c
    gui_events.c msg_box.c msg_note.c front_keys.c kbd_rollover.c
    mic_keys.c clock.c bat_est.c bat_watch.c info_bar.c smeter.c
    band_names.cpp tx_meters.c
    audio.c mfk_control.cpp cw.cpp cw_decoder.c cw_parse.c text_panel.cpp
    rtty.c screen_shot.c screen_rec.c avi_writer.c screen_light.c gps.c cat.cpp tci_ws.c tci_audio.c tci_server.c cfg_socket.cpp
    app_window.c settings_window.cpp swrscan_window.c ft8_window.c
    freq_entry.c gps_window.c cw_messages.c
    voice_messages.c recorder_window.c qth_entry.c station.c dialog_channels.c
    js8_window.c js8_map.c map_geo.c
    dialog_psk.c psk_rx.c psk_tx.c psk_spec.c psk_macro.c digi_qso.c
    dialog_olivia.c olivia.c olivia_app.c olivia_demod.c olivia_macro.c olivia_mod.c olivia_resamp.c
    olivia_rx.c olivia_tx.c
    dialog_jtty.c jtty.c jtty_rx.c jtty_tx.c
    digi_textlog.c
    tx_player.c tx_level.c
    swr_guard.c tune_tx.c ext_tune.c
    base_guard.c
    app_sstv.c
    app_sstv_tx.c
    app_sstv_gallery.c
    dialog_cluster.c
    freq_list.c
    ft8_udp.c
    pskreporter.c
    dxcc.c
    dialog_modes.c
    apps_menu.c
    birdie.c dialog_birdie.c
    dialog_cw_trainer.c
    dialog_cw_kbd.c
    theme.c
    freq_strip.c
    freq_scale.c
    spot_marks.c
    band_plan.c
    itu_region.c
    cw_field.c
    paddle_keyer.c
    cluster_column.c
    sstv/sstv_mode.c
    sstv/sstv_vis.c
    sstv/sstv_sync.c
    sstv/sstv_image.c
    sstv/sstv_tx.c
    sstv/sstv_png.c
    sstv/denoise.c
    wefax_window.c wefax/decoder.c wefax/encoder.c
    navtex_window.c navtex/sitorb.c navtex/dsc.c
    channel_list.c broadcast_db.c cJSON.c
    text_popup.c cw_encoder.c image_level.c start_trace.c
    button_bar.cpp vol_control.cpp recorder.c
    cw_tune_view.c adif.c qso_log.c scheduler.cpp
    wifi_window.c wifi.cpp btaudio.cpp bt_dev.c radio_controls.cpp
    usb_devices.cpp knob_info.cpp
    dialog_wspr.c app_frame.c
    dialog_field.c
    dialog_bands.c
    dialog_snake.c
    remote_server.c
    control_owner.c
    winlink.c
)

# The radio behind src/hw/hw.h (HW_BACKEND, chosen in the top-level CMakeLists.txt)
if(HW_BACKEND STREQUAL "x6100")
    target_sources(${PROJECT_NAME} PUBLIC hw/x6100/hw_x6100.c)
    set(HW_LIBS aether_x6100_control)
elseif(HW_BACKEND STREQUAL "x6200")
    target_sources(${PROJECT_NAME} PUBLIC hw/x6200/hw_x6200.c)
    set(HW_LIBS aether_x6200_control)
elseif(HW_BACKEND STREQUAL "stub")
    target_sources(${PROJECT_NAME} PUBLIC hw/stub/hw_stub.c)
    set(HW_LIBS m)
else()
    message(FATAL_ERROR "HW_BACKEND must be x6100, x6200 or stub, not '${HW_BACKEND}'")
endif()

# Subfolders: some add their sources to the GUI, others build a library
foreach(dir fonts ft8 js8 widgets params qth cluster field wspr cfg cw_decoder)
    add_subdirectory(${dir})
endforeach()

# ft8_lib headers (buildroot package ft8lib; the host build has them in build-host/deps);
# only for the GUI itself, not the libraries above
if(NOT HOST_BUILD)
    include_directories(${CMAKE_SYSROOT}/usr/include/ft8lib/)
endif()

# The panel on a PC
if(HOST_BUILD)
    target_sources(${PROJECT_NAME} PUBLIC host/host_panel.c)
endif()

# System libraries: threads with -pthread, the rest through pkg-config
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    libnm dbus-glib-1 sqlite3 libpng sndfile libudev libpulse libpulse-simple alsa libjpeg
)
# gpsd's library: the radio has it; a PC without it builds without GPS (gps.c)
pkg_check_modules(gps IMPORTED_TARGET libgps)
if(gps_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_LIBGPS=1)
    set(GPS_LIBS PkgConfig::gps)
endif()

# Link order matters for the static libraries: ours first, then LVGL, the
# radio backend's library, liquid-dsp and ft8_lib
target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::deps ${GPS_LIBS}
    FT8 QTH CLUSTER FIELD WSPR CW_DECODER JS8 cfg
    Threads::Threads
    lvgl lvgl::drivers
    ${HW_LIBS} liquid ft8
)

# For a debug build on the radio add to the target, compile and link:
#   -g -O0 -fno-omit-frame-pointer -fasynchronous-unwind-tables (link: -g -rdynamic)
# and for memory errors -fsanitize=address -fsanitize=undefined -fno-sanitize-recover
# (link also -static-libasan -static-libubsan).
