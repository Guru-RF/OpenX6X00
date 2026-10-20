/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

/* Minimal AVI (RIFF) writer: one MJPEG video stream + one PCM s16 mono audio
 * stream, with an idx1 index. Headers are patched in avi_close(). */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct avi_t avi_t;

avi_t * avi_open(const char *path, uint16_t width, uint16_t height, uint16_t fps, uint32_t rate);
bool    avi_write_video(avi_t *avi, const uint8_t *jpeg, size_t size);
bool    avi_write_audio(avi_t *avi, const int16_t *samples, size_t nsamples);
/* Bytes written so far */
uint32_t avi_size(avi_t *avi);
/* Writes the index, fixes the headers, closes the file. false: a write failed (disk full) */
bool    avi_close(avi_t *avi);
