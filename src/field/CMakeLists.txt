add_library(FIELD STATIC refs.c flog.c rbn.c bands.c qsy.c)
target_link_libraries(FIELD PUBLIC CLUSTER m)
