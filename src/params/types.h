#pragma once
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>


// /*********************
//  *  DEFINE BASE TYPES
//  *********************/

// #define TYPEDEF_PARAM_STRUCT(T) typedef struct {\
//     T##_t   x; \
//     bool    dirty; \
//     char    *name; \
//     char    *voice; \
// } param_##T##_t;


// TYPEDEF_PARAM_STRUCT(int16)
// TYPEDEF_PARAM_STRUCT(uint16)
// TYPEDEF_PARAM_STRUCT(int32)
// TYPEDEF_PARAM_STRUCT(uint64)

/* Params items */

typedef struct {
    char        *name;
    bool        x;
    bool        dirty;
} params_bool_t;

typedef struct {
    char        *name;
    float       x;
    bool        dirty;
} params_float_t;

typedef struct {
    char        *name;
    uint8_t     x;
    uint8_t     min;
    uint8_t     max;
    bool        dirty;
} params_uint8_t;

typedef struct {
    char        *name;
    int8_t      x;
    bool        dirty;
} params_int8_t;

typedef struct {
    char        *name;
    uint16_t    x;
    bool        dirty;
} params_uint16_t;

typedef struct {
    char        *name;
    int16_t     x;
    bool        dirty;
} params_int16_t;

typedef struct {
    char        *name;
    int32_t     x;
    bool        dirty;
} params_int32_t;

typedef struct {
    char        *name;
    uint64_t    x;
    bool        dirty;
} params_uint64_t;

typedef struct {
    char        *name;
    char        x[32];
    uint8_t     max_len;
    bool        dirty;
} params_str_t;
