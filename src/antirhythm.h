#ifndef AR_ANTIRHYTHM_H
#define AR_ANTIRHYTHM_H

#include <stdint.h>

/* The pulse is the song's beat folded by octaves into this range, so it never
 * goes faster than 100 BPM (about the comfortable limit for steady tapping). */
#define AR_PULSE_MIN 0.6 /* seconds, 100 BPM */
#define AR_PULSE_MAX 1.2 /* seconds,  50 BPM */

typedef enum
{
   AR_DRIFT_SUBTLE = 0, /* drifts steadily to half a beat off by the end */
   AR_DRIFT_WOBBLY,     /* two full in-and-out phase cycles over the song */
   AR_DRIFT_CHAOTIC     /* five phase cycles */
} ar_drift;

typedef struct
{
   ar_drift drift;
   double   fail_window;   /* seconds: |error| beyond this fails */
   double   near_window;   /* seconds: |error| up to this is Perfect */
   int      lead_in_beats; /* visual-only targets before the song starts */
   double   fade_in;       /* seconds */
} ar_params;

/* Everything is on the game clock: t = 0 is the start of the lead-in. */
typedef struct
{
   double song_bpm;      /* detected */
   double song_period;   /* 60 / song_bpm */
   double folded;        /* song period folded into [AR_PULSE_MIN, AR_PULSE_MAX] */
   double delta;         /* relative detune, always > 0 */
   int    sign;          /* +1 slower than the song, -1 faster */
   double interval;      /* anti-interval T' = folded * (1 + sign * delta) */
   double first_target;  /* time of target 0 */
   double audio_start;   /* game time at which song sample 0 plays */
   double song_length;   /* seconds */
   int    lead_in;       /* targets [0, lead_in) are practice */
   int    n_targets;     /* total, including lead-in */
} ar_chart;

double ar_fold_period(double period);
double ar_solve_delta(ar_drift drift, int song_targets);
void   ar_chart_build(ar_chart *c, double song_bpm, double first_beat, double song_length,
                      const ar_params *p, uint32_t seed);
double ar_target_time(const ar_chart *c, int index);

typedef enum
{
   AR_PLAYING = 0,
   AR_FAILED,
   AR_CLEARED
} ar_status;

typedef enum
{
   AR_FAIL_NONE = 0,
   AR_FAIL_MISS,      /* tap too far from its target */
   AR_FAIL_SKIPPED,   /* target passed with no tap */
   AR_FAIL_EXTRA      /* second tap for an already-hit target */
} ar_fail_reason;

typedef enum
{
   AR_GRADE_NONE = 0,
   AR_GRADE_PERFECT,
   AR_GRADE_NEAR,
   AR_GRADE_PRACTICE_MISS /* off-beat tap during the lead-in; never fatal */
} ar_grade;

typedef struct
{
   ar_status      status;
   ar_fail_reason fail_reason;
   int            fail_target;
   int            next;       /* first target not yet resolved */
   int            perfect;
   int            near;
   int            combo;
   ar_grade       last_grade;
   double         last_error; /* seconds, negative = early */
   double         last_tap;   /* game time of the last tap, -1 if none */
} ar_judge;

void ar_judge_reset(ar_judge *j);
/* Resolve targets whose window closed before time t. */
void ar_judge_advance(ar_judge *j, const ar_chart *c, const ar_params *p, double t);
/* Register a tap at game time t (already latency-corrected). */
void ar_judge_tap(ar_judge *j, const ar_chart *c, const ar_params *p, double t);

#endif
