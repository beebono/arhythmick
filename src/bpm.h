#ifndef AR_BPM_H
#define AR_BPM_H

#include <stddef.h>
#include <stdint.h>

#define AR_BPM_MIN 60.0
#define AR_BPM_MAX 200.0

typedef struct
{
   double bpm;          /* most common tempo, refined over the whole song */
   double first_beat;   /* seconds: first point of the beat grid (phase) */
   int    windows;      /* number of analysis windows */
   int    agreeing;     /* windows whose estimate fell in the winning bin */
} ar_tempo;

/* Analyzes interleaved stereo s16 at `rate`. Returns 0 on success. */
int ar_tempo_analyze(const int16_t *stereo, size_t frames, unsigned rate, ar_tempo *out);

#endif
