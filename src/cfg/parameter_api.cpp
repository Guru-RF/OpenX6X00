#include "parameter_api.h"

#include <string>

#include "parameter.h"

int32_t param_i_get(const ParamInt *p) {
    return p->get();
}

void param_i_set(ParamInt *p, int32_t v) {
    p->set(v);
}

float param_f_get(const ParamFloat *p) {
    return p->get();
}
