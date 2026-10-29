/* Host test shim of src/audio.h: what src/winlink.c calls */
#pragma once
#include <stdbool.h>
#define AUDIO_PLAY_RATE (44100)
void audio_play_flush(void);
bool audio_play_lowlat_begin(void);
void audio_play_lowlat_end(void);
