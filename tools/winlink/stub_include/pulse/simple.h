/* Host test shim of PulseAudio's simple API: stubs.c gives the record stream the other station's air, or a
 * tone, at the rate asked and on the wall clock */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct pa_simple pa_simple;
typedef enum { PA_SAMPLE_S16NE = 3 } pa_sample_format_t;
typedef enum { PA_STREAM_PLAYBACK = 1, PA_STREAM_RECORD = 2 } pa_stream_direction_t;
typedef struct { pa_sample_format_t format; uint32_t rate; uint8_t channels; } pa_sample_spec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } pa_buffer_attr;
pa_simple *pa_simple_new(const char *server, const char *name, pa_stream_direction_t dir, const char *dev,
                         const char *stream_name, const pa_sample_spec *ss, const void *map,
                         const pa_buffer_attr *attr, int *error);
int  pa_simple_read(pa_simple *s, void *data, size_t bytes, int *error);
void pa_simple_free(pa_simple *s);
