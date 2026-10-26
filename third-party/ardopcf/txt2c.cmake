# A text file as a C string, the way upstream's lib/txt2c makes it (carriage
# returns dropped, a tab as two spaces), without a program to build for the
# build machine first:
#   cmake -DIN=webgui/webgui.js -DOUT=gen-webgui.js.c -DVAR=webgui_js -P txt2c.cmake
# The trailing 0x00 ends the string: Webgui.c takes its strlen()
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
string(REPLACE "0x0d," "" bytes "${bytes}")
string(REPLACE "0x09," "0x20,0x20," bytes "${bytes}")
get_filename_component(name "${IN}" NAME)
file(WRITE "${OUT}" "// Made from ${name} by txt2c.cmake\n\nconst char ${VAR}[] = {${bytes}0x00};\n")
