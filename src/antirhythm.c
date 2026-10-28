#include "antirhythm.h"

#include <math.h>

#define DELTA_MAX 0.15

double ar_fold_period(double period)
{
   if (period <= 0.0)
      return 0.75;
   while (period < AR_PULSE_MIN)
      period *= 2.0;
   while (period > AR_PULSE_MAX)
      period *= 0.5;
   return period;
}

/* A player who taps along with the song (in phase with target `lead_in` when
 * the song starts) is off by k * delta * folded seconds at song target k.
 *
 * Subtle:  total drift = half a beat. The follower starts in sync, slides
 *          steadily out of the window and ends fully off the beat, never
 *          coming back in.
 * Wobbly:  total drift = two beats, so the song slides out of phase and back
 *          in twice.
 * Chaotic: five full cycles. */
double ar_solve_delta(ar_drift drift, int song_targets)
{
   double steps = song_targets > 1 ? (double)(song_targets - 1) : 1.0;
   double delta;

   switch (drift)
   {
      case AR_DRIFT_WOBBLY:  delta = 2.0 / steps; break;
      case AR_DRIFT_CHAOTIC: delta = 5.0 / steps; break;
      case AR_DRIFT_SUBTLE:
      default:               delta = 0.5 / steps; break;
   }
   if (delta > DELTA_MAX)
      delta = DELTA_MAX;
   return delta;
}

static uint32_t xorshift32(uint32_t x)
{
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   return x;
}

static int count_song_targets(double interval, double first_beat, double song_length,
      double fail_window)
{
   /* song targets sit at song time first_beat + k * interval; the last one
    * must have its whole window inside the song */
   double usable = song_length - first_beat - fail_window;
   if (usable < 0.0)
      return 0;
   return (int)floor(usable / interval) + 1;
}

void ar_chart_build(ar_chart *c, double song_bpm, double first_beat, double song_length,
      const ar_params *p, uint32_t seed)
{
   int n_song;

   c->song_bpm    = song_bpm;
   c->song_period = 60.0 / song_bpm;
   c->folded      = ar_fold_period(c->song_period);
   c->song_length = song_length;
   c->lead_in     = p->lead_in_beats;
   c->sign        = (xorshift32(seed ? seed : 0x9e3779b9u) & 1) ? 1 : -1;

   /* the folded grid is a subset (or superset) of the song's beats, so its
    * phase is the song's first beat folded into one folded period */
   while (first_beat >= c->folded)
      first_beat -= c->folded;

   n_song      = count_song_targets(c->folded, first_beat, song_length, p->fail_window);
   c->delta    = ar_solve_delta(p->drift, n_song);
   c->interval = c->folded * (1.0 + c->sign * c->delta);

   /* recount with the real interval, then re-solve so the drift still spans
    * the song exactly */
   n_song      = count_song_targets(c->interval, first_beat, song_length, p->fail_window);
   c->delta    = ar_solve_delta(p->drift, n_song);
   c->interval = c->folded * (1.0 + c->sign * c->delta);
   n_song      = count_song_targets(c->interval, first_beat, song_length, p->fail_window);

   c->first_target = c->interval;
   /* song beat `first_beat` lands exactly on target `lead_in` */
   c->audio_start  = c->first_target + c->lead_in * c->interval - first_beat;
   c->n_targets    = c->lead_in + n_song;
}

double ar_target_time(const ar_chart *c, int index)
{
   return c->first_target + index * c->interval;
}

void ar_judge_reset(ar_judge *j)
{
   j->status      = AR_PLAYING;
   j->fail_reason = AR_FAIL_NONE;
   j->fail_target = -1;
   j->next        = 0;
   j->perfect     = 0;
   j->near        = 0;
   j->combo       = 0;
   j->last_grade  = AR_GRADE_NONE;
   j->last_error  = 0.0;
   j->last_tap    = -1.0;
}

static void fail(ar_judge *j, ar_fail_reason reason, int target)
{
   j->status      = AR_FAILED;
   j->fail_reason = reason;
   j->fail_target = target;
   j->combo       = 0;
}

void ar_judge_advance(ar_judge *j, const ar_chart *c, const ar_params *p, double t)
{
   if (j->status != AR_PLAYING)
      return;
   while (j->next < c->n_targets && t > ar_target_time(c, j->next) + p->fail_window)
   {
      if (j->next >= c->lead_in)
      {
         fail(j, AR_FAIL_SKIPPED, j->next);
         return;
      }
      j->next++; /* practice targets may be skipped */
   }
   if (j->next >= c->n_targets)
      j->status = AR_CLEARED;
}

void ar_judge_tap(ar_judge *j, const ar_chart *c, const ar_params *p, double t)
{
   int k;
   double err;

   ar_judge_advance(j, c, p, t);
   if (j->status != AR_PLAYING)
      return;

   k = (int)floor((t - c->first_target) / c->interval + 0.5);
   if (k < 0)
      k = 0;
   if (k >= c->n_targets)
      k = c->n_targets - 1;
   if (k > j->next)
      k = j->next; /* cannot happen while fail_window < interval / 2 */

   err           = t - ar_target_time(c, k);
   j->last_tap   = t;
   j->last_error = err;

   if (k < j->next)
   {
      /* this target was already resolved */
      if (k < c->lead_in)
         j->last_grade = AR_GRADE_PRACTICE_MISS;
      else
         fail(j, AR_FAIL_EXTRA, k);
      return;
   }

   if (fabs(err) > p->fail_window)
   {
      if (k < c->lead_in)
         j->last_grade = AR_GRADE_PRACTICE_MISS;
      else
         fail(j, AR_FAIL_MISS, k);
      return;
   }

   j->last_grade = fabs(err) <= p->near_window ? AR_GRADE_PERFECT : AR_GRADE_NEAR;
   if (k >= c->lead_in)
   {
      if (j->last_grade == AR_GRADE_PERFECT)
         j->perfect++;
      else
         j->near++;
      j->combo++;
   }
   j->next = k + 1;
   if (j->next >= c->n_targets)
      j->status = AR_CLEARED;
}
