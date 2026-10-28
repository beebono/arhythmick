#ifndef AR_DECODE_H
#define AR_DECODE_H

#include <stddef.h>
#include <stdint.h>

#define AR_OUTPUT_RATE 44100

/* Interleaved stereo s16 at AR_OUTPUT_RATE. */
typedef struct
{
   int16_t *samples;
   size_t   frames;
} ar_audio;

/* Decodes mp3/ogg/flac/wav (chosen by extension, then by trial), downmixes or
 * upmixes to stereo and resamples to AR_OUTPUT_RATE. Returns 0 on success. */
int  ar_decode_file(const char *path, ar_audio *out);
void ar_audio_free(ar_audio *a);

#endif
