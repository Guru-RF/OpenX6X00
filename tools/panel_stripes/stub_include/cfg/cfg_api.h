/* Host test shim (tools/panel_stripes): the setting panel_stripes.c follows, the test's own */
#pragma once

#include <stdint.h>

typedef struct ParamInt ParamInt;
typedef struct Subject  Subject;
typedef struct Observer Observer;
typedef void (*observer_cb)(Subject *, void *);

extern ParamInt *cfg_panel_stripes;

int32_t   param_i_get(const ParamInt *p);
Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data);
