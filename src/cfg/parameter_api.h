#pragma once

// C-compatible API for the Parameter<ValueType, DbType, Scale> template. Typed
// get/set only: route through Parameter<T>::set / SubjectT<T>::get, so
// validators, deferred-write enqueue and observer notifications all apply.
//
// The opaque handles below resolve to the concrete Parameter<int32_t> /
// Parameter<float> / Parameter<std::string> instances owned by the static
// SettingsManager (cfg_api.cpp). Creation stays C++-only (validators and
// storage wiring are C++ constructs).

#include <stdint.h>

#ifdef __cplusplus
#include "parameter.h"

using ParamInt   = Parameter<int32_t>;
// Every float setting is stored scaled (Parameter<float, int32_t, N>, N differs),
// so the C handle is a read-only view of the value: no cast to a wrong scale,
// no set() that would write the float unscaled. Set them from C++ through the
// typed cfg_sm member.
using ParamFloat = const SubjectT<float>;
using ParamText  = Parameter<std::string>;
#else
// Opaque C handles (resolved to real C++ types in C++ builds).
typedef struct ParamInt   ParamInt;
typedef struct ParamFloat ParamFloat;
typedef struct ParamText  ParamText;
#endif

#ifdef __cplusplus
extern "C" {
#endif

int32_t     param_i_get(const ParamInt *p);
void        param_i_set(ParamInt *p, int32_t v);
float       param_f_get(const ParamFloat *p);

#ifdef __cplusplus
} // extern "C"
#endif
