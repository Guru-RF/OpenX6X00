#include "parameter_api.h"

#include <cstdio>
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

size_t param_t_get(const ParamText *p, char *out, size_t size) {
    std::string v = p->get();

    if (size) {
        snprintf(out, size, "%s", v.c_str());
    }
    return v.size();
}

void param_t_set(ParamText *p, const char *v) {
    p->set(v ? v : "");
}
