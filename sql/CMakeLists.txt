# Default settings database, built from params.sql and installed as
# /usr/share/x6100/params.default.db; S95gui copies it to /mnt/params.db when
# that is missing.
# Copyright (c) 2026 Giovanni Ricciardi IU1WXY

set(PARAMS_DB ${CMAKE_BINARY_DIR}/params.db)

add_custom_command(
    OUTPUT ${PARAMS_DB}
    MAIN_DEPENDENCY params.sql
    COMMAND ${CMAKE_COMMAND} -E rm -f ${PARAMS_DB}
    COMMAND sqlite3 ${PARAMS_DB} < params.sql
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
add_custom_target(params_sqlite ALL DEPENDS ${PARAMS_DB} SOURCES params.sql)

install(FILES ${PARAMS_DB} DESTINATION share/x6100 RENAME params.default.db)
