# The GUI program, x6100_gui. Copyright (c) 2026 Giovanni Ricciardi IU1WXY
# The source order below is the link order: keep it when adding files.
add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PUBLIC
    gui_main.c gui_wake.c ops_room.c perf_hud.c load_meter.c load_stat.c screen_main.c walkie.c gui_styles.c dropdown_fit.c mono.c wf_palettes.c
    spectrum_view.c radio.c dsp.cpp util.cpp
    waterfall_view.c wf3d_shape.c closeup_view.c knob_device.c key_input.c input_caps.c evdev_sync.c mfk_device.c
    gui_events.c msg_box.c msg_note.c front_keys.c kbd_rollover.c kbd_hyper.c kbd_keymap.c kbd_layout.c kbd_ptt.c
    mic_keys.c clock.c bat_est.c bat_watch.c info_bar.c smeter.c audio_meter.c
    band_names.cpp tx_meters.c
    audio.c audio_owner.c mfk_control.cpp cw.cpp cw_decoder.c cw_parse.c text_panel.cpp
    rtty.c rtty_tx.c rtty_send.c rtty_macro.c screen_shot.c screen_rec.c avi_writer.c screen_light.c screen_idle.c screen_saver.c gps.c
    cat.cpp tci_ws.c tci_audio.c tci_server.c tci_ask.c cfg_socket.cpp wavelog_cat.c
    app_window.c settings_window.cpp settings_place.c swrscan_window.c ft8_window.c
    freq_entry.c gps_window.c cw_messages.c
    voice_messages.c recorder_window.c qth_entry.c station.c dialog_channels.c
    js8_window.c js8_map.c map_geo.c ghostnet.c ghostnet_gui.c ghostnet_tr.c
    dialog_psk.c psk_rx.c psk_tx.c psk_spec.c psk_macro.c digi_qso.c
    dialog_olivia.c olivia.c olivia_app.c olivia_demod.c olivia_macro.c olivia_mod.c olivia_resamp.c
    olivia_rx.c olivia_tx.c
    dialog_jtty.c jtty.c jtty_rx.c jtty_tx.c
    digi_freqs.c
    digi_textlog.c
    tx_player.c tx_level.c
    swr_guard.c tune_tx.c ext_tune.c
    base_guard.c cpu_power.c
    app_sstv.c
    app_sstv_tx.c
    app_sstv_gallery.c
    dialog_cluster.c
    freq_list.c
    ft8_udp.c
    pskreporter.c
    freedv_report.c
    dxcc.c countries.c
    dialog_modes.c
    apps_menu.c
    birdie.c dialog_birdie.c dialog_flights.c
    dialog_zello.c zello.c voice_net.c voice_link.c voice_ptt.c ctcss.c dialog_svx.c svx.c svx_proto.c
    dialog_dmr.c dmr_ambe.c dmr_proto.c live_map.c
    dialog_meteo.c meteo.c meteo_alerts.c
    first_start.c first_start_rule.c first_start_kbd.c
    dialog_cw_trainer.c cw_trainer_score.c
    dialog_cw_kbd.c recolor_rows.c
    theme.c
    freq_strip.c main_call.c boot_cw.c
    lcd_panel.c
    panel_stripes.c
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
    wefax_window.c wefax_bulletin.c wefax_viewer.c wefax_night.c wefax/decoder.c wefax/encoder.c wefax/plan.c
    wefax/quality.c wefax/thumb.c wefax/tilt.c wefax/night.c clock_sync.c clock_rule.c time_station.c
    navtex_window.c navtex/sitorb.c navtex/dsc.c
    dialog_freedv.c freedv_rx.c freedv_text.c freedv_tx.c
    channel_list.c broadcast_db.c cJSON.c
    text_popup.c cw_encoder.c image_level.c start_trace.c card_space.c card_space_notice.c
    cw_beacon.c
    button_bar.cpp button_page.c vol_control.cpp recorder.c
    cw_tune_view.c adif.c qso_log.c scheduler.cpp
    wifi_window.c wifi.cpp btaudio.cpp bt_dev.c radio_controls.cpp
    usb_devices.cpp knob_info.cpp
    wr12.c pa_bias.c
    dialog_wspr.c app_frame.c
    dialog_field.c field_map.c
    dialog_bands.c
    sun.c prop_map.c conditions.c beacons.c dialog_propagation.c
    scan.c dialog_scan.c band_keep.c
    aero.c dialog_aero.c cb.c dialog_cb.c
    dialog_snake.c
    dialog_iradio.c iradio_list.c
    remote_server.c
    control_owner.c
    winlink.c
    i18n.cpp
    voice.cpp voice_focus.c voice_text.c speech_engine.c
    winlink_mail.c
)

# Spoken feedback (voice.cpp): RHVoice is not linked. speech_engine.c opens its library when speech is first asked for
# (dlopen), with the C interface of third-party/rhvoice/RHVoice.h, so the same GUI runs on a card image without it
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/third-party/rhvoice)
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})

# The languages (i18n.h): every GUI source gets i18n_hook.h first, so the texts it hands to LVGL are translated.
# Only this target's sources: LVGL's own and the libraries below do not
target_compile_options(${PROJECT_NAME} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/i18n_hook.h)

# The X6200's base firmware screen (tools/base_update): built for the X6200 only, never in another radio's GUI
if(HW_BACKEND STREQUAL "x6200")
    target_sources(${PROJECT_NAME} PRIVATE dialog_base_update.c)
endif()

# The radio behind src/hw/hw.h (HW_BACKEND, chosen in the top-level CMakeLists.txt)
if(HW_BACKEND STREQUAL "x6100")
    target_sources(${PROJECT_NAME} PUBLIC hw/x6100/hw_x6100.c hw/x6100/test_base.c)
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

# ft8_lib headers (buildroot package ft8lib; the host build has them in HOST_DEPS, build-host/deps by default);
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
# gio-unix-2.0: the sockets BlueZ hands over with a D-Bus call (bt_dev.c, TCI over Bluetooth)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    libnm dbus-glib-1 sqlite3 libpng sndfile libudev libpulse libpulse-simple alsa libjpeg gio-unix-2.0
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
    FT8 QTH CLUSTER FIELD WSPR CW_DECODER JS8 cfg mbe
    Threads::Threads
    lvgl lvgl::drivers
    ${HW_LIBS} liquid ft8
)

# RADE (FreeDV): freedv/rade_c and the Opus fork with FARGAN, built outside by tools/rade/build.sh
# (include/rade and include/opus with the headers, lib/librade.a and lib/libopus.a). Without it the RADE app
# opens and says so; freedv_rx.c is empty. Opus's DNN symbols are looked up by name inside the
# libraries, hence --whole-archive for libopus, as rade_c's own CMake does
set(RADE_PREFIX "" CACHE PATH "RADE built by tools/rade/build.sh (empty: the app without a decoder)")
if(RADE_PREFIX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RADE_ENABLED=1)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${RADE_PREFIX}/include/rade ${RADE_PREFIX}/include/opus ${RADE_PREFIX}/include/opus/dnn
        ${RADE_PREFIX}/include/opus/celt ${RADE_PREFIX}/include/opus/include)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${RADE_PREFIX}/lib/librade.a
        -Wl,--whole-archive ${RADE_PREFIX}/lib/libopus.a -Wl,--no-whole-archive m)
endif()

# For a debug build on the radio add to the target, compile and link:
#   -g -O0 -fno-omit-frame-pointer -fasynchronous-unwind-tables (link: -g -rdynamic)
# and for memory errors -fsanitize=address -fsanitize=undefined -fno-sanitize-recover
# (link also -static-libasan -static-libubsan).
