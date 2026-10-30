/* Host test shim of src/params/params.h: the level the apps share, the playback gain, the Line-in; the apps' TX
 * offsets and actions (ext_tune.c) */
#pragma once
#include <stdint.h>
typedef struct {
    float x;
} params_float_t;
typedef struct {
    uint16_t x;
} params_uint16_t;
typedef struct {
    params_float_t  ft8_output_gain_offset, play_gain_db_f;
    uint8_t         line_in;
    uint16_t        rtty_center;
    params_uint16_t ft8_tx_freq, js8_tx_freq;
} params_t;
extern params_t params;
void params_float_set(params_float_t *p, float v);
typedef enum {
    ACTION_NONE     = 0,
    ACTION_APP_RTTY = 100, ACTION_APP_FT8, ACTION_APP_WEFAX = 109, ACTION_APP_NAVTEX, ACTION_APP_JS8, ACTION_APP_SSTV,
    ACTION_APP_WSPR = 116, ACTION_APP_PSK = 121, ACTION_APP_OLIVIA, ACTION_APP_JTTY,
} press_action_t;
