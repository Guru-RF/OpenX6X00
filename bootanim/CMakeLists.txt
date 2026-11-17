# x6100-bootanim: the boot logo composes itself while the radio boots (see bootanim.c). The boot sound's text and its
# codes are the GUI's (src/boot_cw.c, src/cw_decoder/morse_code.c)
find_package(PNG REQUIRED)
add_executable(x6100-bootanim bootanim.c ${CMAKE_SOURCE_DIR}/src/boot_cw.c
               ${CMAKE_SOURCE_DIR}/src/cw_decoder/morse_code.c)
if(HW_BACKEND STREQUAL "x6200")
    set(BOOT_CONTROL aether_x6200_control)
else()
    set(BOOT_CONTROL aether_x6100_control)
endif()
target_link_libraries(x6100-bootanim PRIVATE PNG::PNG ${BOOT_CONTROL} gpiod asound sqlite3 pthread m)
install(TARGETS x6100-bootanim DESTINATION sbin)
install(FILES ${CMAKE_SOURCE_DIR}/boot/logo.png DESTINATION share/x6100 RENAME bootlogo.png)
