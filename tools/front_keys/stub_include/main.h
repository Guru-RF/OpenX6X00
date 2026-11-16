/* tools/front_keys: the MFK encoder, whose press front_keys.c sets */
#pragma once
#include <stdbool.h>
typedef struct {
    bool pressed;
} encoder_t;
extern encoder_t *mfk;
