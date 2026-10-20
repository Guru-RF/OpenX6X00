# btcat: Bluetooth SPP <-> CAT bridge (see btcat.c)
add_executable(btcat btcat.c)
install(TARGETS btcat DESTINATION sbin)
