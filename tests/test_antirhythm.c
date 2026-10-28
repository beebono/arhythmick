#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/antirhythm.h"
#include "../src/bpm.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
   if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static ar_params default_params(void)
{
   ar_params p;
   p.drift         = AR_DRIFT_SUBTLE;
   p.fail_window   = 0.100;
   p.near_window   = 0.050;
   p.lead_in_beats = 8;
   p.fade_in       = 3.0;
   return p;
}

static void test_fold(void)
{
   CHECK(fabs(ar_fold_period(60.0 / 120.0) - 1.0) < 1e-9, "120 bpm -> 60");
   CHECK(fabs(ar_fold_period(60.0 / 100.0) - 0.6) < 1e-9, "100 bpm stays");
   CHECK(fabs(ar_fold_period(60.0 / 60.0) - 1.0) < 1e-9, "60 bpm");
   CHECK(fabs(ar_fold_period(60.0 / 174.0) - 120.0 / 174.0) < 1e-9, "174 bpm -> 87");
   CHECK(fabs(ar_fold_period(60.0 / 45.0) - 30.0 / 45.0) < 1e-9, "45 bpm -> 90");
   CHECK(fabs(ar_fold_period(60.0 / 128.0) - 120.0 / 128.0) < 1e-9, "128 bpm -> 64");
}

/* Fraction of song targets a player tapping exactly with the song would hit,
 * evaluating every tap instead of stopping at the first miss. */
static double follower_hit_rate(const ar_chart *c, double first_beat, double w)
{
   int k, hits = 0, n_song = c->n_targets - c->lead_in;
   while (first_beat >= c->folded)
      first_beat -= c->folded;
   for (k = 0; k < n_song; k++)
   {
      double t  = c->audio_start + first_beat + k * c->folded;
      int    i  = (int)floor((t - c->first_target) / c->interval + 0.5);
      double e  = t - ar_target_time(c, i);
      if (fabs(e) <= w)
         hits++;
   }
   return (double)hits / n_song;
}

static void test_chart(void)
{
   ar_params p = default_params();
   double bpms[]    = { 90.0, 128.0, 174.0, 70.0 };
   double lengths[] = { 30.0, 180.0, 240.0 };
   size_t a, b;

   for (a = 0; a < sizeof(bpms) / sizeof(bpms[0]); a++)
      for (b = 0; b < sizeof(lengths) / sizeof(lengths[0]); b++)
      {
         ar_chart c;
         double rate, first_beat = 0.37;
         ar_chart_build(&c, bpms[a], first_beat, lengths[b], &p, (uint32_t)(a * 7 + b + 1));

         CHECK(c.folded >= AR_PULSE_MIN && c.folded <= AR_PULSE_MAX, "folded %f", c.folded);
         CHECK(c.audio_start > 0.0, "audio_start %f", c.audio_start);
         {
            /* song beat lands on the first song target */
            double fb = first_beat;
            while (fb >= c.folded) fb -= c.folded;
            CHECK(fabs(c.audio_start + fb - ar_target_time(&c, c.lead_in)) < 1e-9, "alignment");
         }
         /* last target fits in the song */
         CHECK(ar_target_time(&c, c.n_targets - 1) + p.fail_window <= c.audio_start + c.song_length + 1e-9,
               "last target past song end");

         /* subtle: drift grows linearly to half a beat, so the follower is
          * inside the window only for the first 2w/T of the song */
         rate = follower_hit_rate(&c, first_beat, p.fail_window);
         CHECK(fabs(rate - 2.0 * p.fail_window / c.folded) <= 0.05,
               "subtle follower hit rate %.3f, expected %.3f (bpm %.0f, len %.0f)",
               rate, 2.0 * p.fail_window / c.folded, bpms[a], lengths[b]);
         /* recounting after the detune can shift the target count by one */
         CHECK(fabs(c.delta * c.folded * (c.n_targets - c.lead_in - 1) - c.folded / 2.0)
               <= c.delta * c.folded + 1e-9, "subtle ends half a beat off");
      }

   /* wobbly / chaotic: still a single fixed interval, larger detune */
   {
      ar_chart s, w, ch;
      p.drift = AR_DRIFT_SUBTLE;  ar_chart_build(&s, 128.0, 0.1, 180.0, &p, 5);
      p.drift = AR_DRIFT_WOBBLY;  ar_chart_build(&w, 128.0, 0.1, 180.0, &p, 5);
      p.drift = AR_DRIFT_CHAOTIC; ar_chart_build(&ch, 128.0, 0.1, 180.0, &p, 5);
      CHECK(s.delta < w.delta && w.delta < ch.delta, "drift ordering %f %f %f", s.delta, w.delta, ch.delta);
      CHECK(s.sign == w.sign, "seeded sign is stable");
   }
}

static void test_judge(void)
{
   ar_params p = default_params();
   ar_chart c;
   ar_judge j;
   int i;

   ar_chart_build(&c, 120.0, 0.0, 20.0, &p, 3);

   /* perfect player clears */
   ar_judge_reset(&j);
   for (i = 0; i < c.n_targets; i++)
   {
      double t = ar_target_time(&c, i) + (i % 2 ? 0.02 : -0.07);
      ar_judge_advance(&j, &c, &p, t);
      ar_judge_tap(&j, &c, &p, t);
   }
   ar_judge_advance(&j, &c, &p, c.audio_start + c.song_length);
   CHECK(j.status == AR_CLEARED, "perfect player status %d reason %d at %d", j.status, j.fail_reason, j.fail_target);
   CHECK(j.perfect + j.near == c.n_targets - c.lead_in, "counted %d+%d", j.perfect, j.near);
   CHECK(j.near > 0 && j.perfect > 0, "both grades used");

   /* lead-in can be ignored entirely, skipping the first song target fails */
   ar_judge_reset(&j);
   ar_judge_advance(&j, &c, &p, ar_target_time(&c, c.lead_in) - 0.2);
   CHECK(j.status == AR_PLAYING, "lead-in skips are free");
   ar_judge_advance(&j, &c, &p, ar_target_time(&c, c.lead_in) + p.fail_window + 0.001);
   CHECK(j.status == AR_FAILED && j.fail_reason == AR_FAIL_SKIPPED && j.fail_target == c.lead_in,
         "skip fails: status %d reason %d", j.status, j.fail_reason);

   /* off-beat tap during lead-in is harmless */
   ar_judge_reset(&j);
   ar_judge_tap(&j, &c, &p, ar_target_time(&c, 2) + 0.2);
   CHECK(j.status == AR_PLAYING && j.last_grade == AR_GRADE_PRACTICE_MISS, "practice miss");

   /* wide miss on a song target fails */
   ar_judge_reset(&j);
   for (i = 0; i < c.lead_in; i++)
      ar_judge_tap(&j, &c, &p, ar_target_time(&c, i));
   ar_judge_tap(&j, &c, &p, ar_target_time(&c, c.lead_in) - 0.15);
   CHECK(j.status == AR_FAILED && j.fail_reason == AR_FAIL_MISS, "wide miss: %d %d", j.status, j.fail_reason);

   /* near miss is allowed */
   ar_judge_reset(&j);
   ar_judge_tap(&j, &c, &p, ar_target_time(&c, c.lead_in) + 0.09);
   CHECK(j.status == AR_PLAYING && j.last_grade == AR_GRADE_NEAR, "near allowed");

   /* double tap fails */
   ar_judge_tap(&j, &c, &p, ar_target_time(&c, c.lead_in) + 0.095);
   CHECK(j.status == AR_FAILED && j.fail_reason == AR_FAIL_EXTRA, "double tap: %d %d", j.status, j.fail_reason);

   /* tap in the gap between two song targets fails */
   ar_judge_reset(&j);
   ar_judge_tap(&j, &c, &p, ar_target_time(&c, c.lead_in));
   ar_judge_tap(&j, &c, &p, ar_target_time(&c, c.lead_in) + c.interval * 0.6);
   CHECK(j.status == AR_FAILED && j.fail_reason == AR_FAIL_MISS, "between beats: %d %d", j.status, j.fail_reason);
}

/* Stereo click track: short decaying 1 kHz blips with an accent every 4 beats. */
static int16_t *click_track(double bpm, double offset, double seconds, size_t *frames)
{
   size_t n = (size_t)(seconds * 44100.0), i;
   int16_t *pcm = (int16_t *)calloc(n * 2, sizeof(int16_t));
   double period = 60.0 / bpm, t;
   int beat = 0;
   for (t = offset; t < seconds; t += period, beat++)
   {
      size_t start = (size_t)(t * 44100.0);
      double amp = beat % 4 == 0 ? 20000.0 : 12000.0;
      for (i = 0; i < 2000 && start + i < n; i++)
      {
         double env = exp(-(double)i / 300.0);
         int16_t v = (int16_t)(amp * env * sin(2.0 * 3.14159265358979 * 1000.0 * i / 44100.0));
         pcm[(start + i) * 2]     = v;
         pcm[(start + i) * 2 + 1] = v;
      }
   }
   *frames = n;
   return pcm;
}

static void test_bpm(void)
{
   double bpms[] = { 90.0, 128.0, 174.0, 100.0, 140.0 };
   size_t a;
   for (a = 0; a < sizeof(bpms) / sizeof(bpms[0]); a++)
   {
      size_t frames;
      double offset = 0.3;
      int16_t *pcm = click_track(bpms[a], offset, 60.0, &frames);
      ar_tempo tempo;
      int rc = ar_tempo_analyze(pcm, frames, 44100, &tempo);
      double period = 60.0 / tempo.bpm, phase_err;
      CHECK(rc == 0, "analyze rc");
      /* octave errors are harmless after folding, but report them */
      {
         /* same tempo up to octaves (a fold boundary can split near-equal values) */
         double oct = log(period * bpms[a] / 60.0) / log(2.0);
         CHECK(fabs(oct - floor(oct + 0.5)) < 0.005, "bpm %.0f detected %.2f", bpms[a], tempo.bpm);
      }
      CHECK(fabs(tempo.bpm - bpms[a]) < 1.0, "bpm %.0f detected %.2f (octave?)", bpms[a], tempo.bpm);
      phase_err = fmod(tempo.first_beat - offset + 10.0 * period, period);
      if (phase_err > period / 2)
         phase_err -= period;
      CHECK(fabs(phase_err) < 0.02, "bpm %.0f phase error %.3f s", bpms[a], phase_err);
      printf("  click %.0f bpm -> %.2f bpm, first beat %.3f s (%d/%d windows agree)\n",
            bpms[a], tempo.bpm, tempo.first_beat, tempo.agreeing, tempo.windows);
      free(pcm);
   }
}

int main(void)
{
   test_fold();
   test_chart();
   test_judge();
   test_bpm();
   if (failures)
   {
      printf("%d check(s) failed\n", failures);
      return 1;
   }
   printf("all tests passed\n");
   return 0;
}
