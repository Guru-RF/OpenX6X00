/* Host test shim of src/tx_player.h: stubs.c plays on the wall clock, into a file and to the other station */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TX_PLAYER_AUDIO_HZ 1325
#define TX_PLAYER_MAX_PWR_W 7.0f
typedef bool (*tx_abort_fn_t)(void *arg);
typedef size_t (*tx_fill_fn_t)(int16_t *buf, size_t n, void *ctx);
float tx_player_base_gain_offset(void);
bool  tx_player_stream(tx_fill_fn_t fill, void *fill_ctx, int32_t offset_hz, float base_gain,
                       tx_abort_fn_t abort, void *abort_ctx);
