#include "bpm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TARGET_RATE   11025.0
#define FFT_SIZE      1024
#define HOP           128
#define ONSET_LAG     0.856 /* fraction of the FFT window, see frame_offset */
#define WINDOW_SEC    8.0
#define HIST_BIN_BPM  0.5
#define HIST_BINS     ((int)((AR_BPM_MAX - AR_BPM_MIN) / HIST_BIN_BPM) + 1)

/* Mono, roughly 11 kHz, normalized to [-1, 1]. */
static float *downmix(const int16_t *stereo, size_t frames, unsigned rate,
      size_t *out_len, double *out_rate)
{
   int decim = (int)floor(rate / TARGET_RATE + 0.5);
   size_t n, i;
   float *mono;

   if (decim < 1)
      decim = 1;
   n    = frames / (size_t)decim;
   mono = (float *)malloc((n ? n : 1) * sizeof(float));
   if (!mono)
      return NULL;

   for (i = 0; i < n; i++)
   {
      const int16_t *s = stereo + i * (size_t)decim * 2;
      int32_t acc = 0;
      int k;
      for (k = 0; k < decim; k++)
         acc += s[k * 2] + s[k * 2 + 1];
      mono[i] = (float)acc / (2.0f * decim * 32768.0f);
   }
   *out_len  = n;
   *out_rate = (double)rate / decim;
   return mono;
}

/* Spectral flux onset envelope, one value per HOP samples, with a local mean
 * removed and half-wave rectified so autocorrelation sees only onsets. */
static float *onset_envelope(const float *mono, size_t len, size_t *out_frames)
{
   float re[FFT_SIZE], im[FFT_SIZE], win[FFT_SIZE];
   float *prev, *cur, *env, *smooth;
   size_t frames, f, i;
   int avg_half = 20; /* about 0.23 s each side at 86 Hz */

   if (len < FFT_SIZE)
      return NULL;
   frames = (len - FFT_SIZE) / HOP + 1;

   env    = (float *)calloc(frames, sizeof(float));
   smooth = (float *)calloc(frames, sizeof(float));
   prev   = (float *)calloc(FFT_SIZE / 2, sizeof(float));
   cur    = (float *)calloc(FFT_SIZE / 2, sizeof(float));
   if (!env || !smooth || !prev || !cur)
   {
      free(env); free(smooth); free(prev); free(cur);
      return NULL;
   }

   for (i = 0; i < FFT_SIZE; i++)
      win[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * i / (FFT_SIZE - 1));

   for (f = 0; f < frames; f++)
   {
      const float *x = mono + f * HOP;
      float flux = 0.0f;
      for (i = 0; i < FFT_SIZE; i++)
      {
         re[i] = x[i] * win[i];
         im[i] = 0.0f;
      }
      ar_fft(re, im, FFT_SIZE);
      for (i = 1; i < FFT_SIZE / 2; i++)
      {
         float mag = (float)log(1.0 + 1000.0 * sqrt(re[i] * re[i] + im[i] * im[i]));
         float d   = mag - prev[i];
         if (f > 0 && d > 0.0f)
            flux += d;
         cur[i] = mag;
      }
      memcpy(prev, cur, (FFT_SIZE / 2) * sizeof(float));
      env[f] = flux;
   }

   for (f = 0; f < frames; f++)
   {
      size_t lo = f > (size_t)avg_half ? f - avg_half : 0;
      size_t hi = f + avg_half < frames ? f + avg_half : frames - 1;
      double sum = 0.0;
      for (i = lo; i <= hi; i++)
         sum += env[i];
      smooth[f] = env[f] - (float)(sum / (double)(hi - lo + 1));
      if (smooth[f] < 0.0f)
         smooth[f] = 0.0f;
   }

   free(env); free(prev); free(cur);
   *out_frames = frames;
   return smooth;
}

/* Gentle preference for tempos near 120 BPM to break octave ties. */
static double tempo_weight(double bpm)
{
   double o = log(bpm / 120.0) / log(2.0);
   return exp(-0.5 * o * o);
}

/* Best tempo for env[start, start+len). Returns BPM or 0 if the window is silent. */
static double window_tempo(const float *env, size_t start, size_t len, double erate)
{
   int lag_min = (int)floor(erate * 60.0 / AR_BPM_MAX);
   int lag_max = (int)ceil(erate * 60.0 / AR_BPM_MIN);
   double score[512];
   double r0 = 0.0, best = 0.0;
   int lag, best_lag = -1;
   size_t i;

   if (lag_max + 2 >= 512 || (size_t)(lag_max + 2) >= len)
      return 0.0;

   for (i = 0; i < len; i++)
      r0 += (double)env[start + i] * env[start + i];
   if (r0 < 1e-9)
      return 0.0;

   for (lag = lag_min - 1; lag <= lag_max + 1; lag++)
   {
      double r = 0.0;
      for (i = 0; i + lag < len; i++)
         r += (double)env[start + i] * env[start + i + lag];
      /* unbiased: compensate for fewer overlapping samples at long lags */
      r *= (double)len / (double)(len - lag);
      score[lag] = r * tempo_weight(60.0 * erate / lag);
   }

   for (lag = lag_min; lag <= lag_max; lag++)
   {
      if (score[lag] > best && score[lag] >= score[lag - 1] && score[lag] >= score[lag + 1])
      {
         best     = score[lag];
         best_lag = lag;
      }
   }
   if (best_lag < 0)
      return 0.0;

   {
      /* parabolic interpolation of the peak for sub-frame lag */
      double a = score[best_lag - 1], b = score[best_lag], c = score[best_lag + 1];
      double denom = a - 2.0 * b + c;
      double off   = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
      double bpm;
      if (off > 0.5)  off = 0.5;
      if (off < -0.5) off = -0.5;
      bpm = 60.0 * erate / (best_lag + off);
      if (bpm < AR_BPM_MIN || bpm > AR_BPM_MAX)
         return 0.0;
      return bpm;
   }
}

static double env_at(const float *env, size_t frames, double idx)
{
   size_t i0;
   double frac;
   if (idx < 0.0 || idx >= (double)(frames - 1))
      return 0.0;
   i0   = (size_t)idx;
   frac = idx - (double)i0;
   return env[i0] + (env[i0 + 1] - env[i0]) * frac;
}

/* Mean onset strength on a beat grid of `period` frames starting at `phase`. */
static double comb_score(const float *env, size_t frames, double period, double phase)
{
   double sum = 0.0, idx;
   int n = 0;
   for (idx = phase; idx < (double)(frames - 1); idx += period, n++)
      sum += env_at(env, frames, idx);
   return n ? sum / n : 0.0;
}

int ar_tempo_analyze(const int16_t *stereo, size_t frames, unsigned rate, ar_tempo *out)
{
   size_t mono_len, env_frames, win_len, win_hop, start;
   double mono_rate, erate, frame_offset;
   float *mono, *env;
   int hist[HIST_BINS];
   double *estimates;
   int n_est = 0, max_est, b, best_bin = 0, best_count = 0;
   double coarse, sum = 0.0, best_score = -1.0, best_period = 0.0, best_phase = 0.0;
   double bpm;
   int cnt = 0;

   memset(out, 0, sizeof(*out));
   mono = downmix(stereo, frames, rate, &mono_len, &mono_rate);
   if (!mono)
      return -1;
   env = onset_envelope(mono, mono_len, &env_frames);
   free(mono);
   if (!env)
      return -1;

   erate        = mono_rate / HOP;
   /* log-compressed flux jumps as soon as a transient enters the trailing
    * edge of the window, so frame i reports an onset near the window's end;
    * ONSET_LAG was measured with click tracks */
   frame_offset = FFT_SIZE * ONSET_LAG / mono_rate;

   win_len = (size_t)(WINDOW_SEC * erate);
   if (win_len > env_frames)
      win_len = env_frames;
   win_hop = win_len / 2 ? win_len / 2 : 1;

   max_est   = (int)(env_frames / win_hop) + 2;
   estimates = (double *)malloc(max_est * sizeof(double));
   if (!estimates)
   {
      free(env);
      return -1;
   }

   for (start = 0; start + win_len <= env_frames && n_est < max_est; start += win_hop)
   {
      double t = window_tempo(env, start, win_len, erate);
      out->windows++;
      if (t > 0.0)
         estimates[n_est++] = t;
   }
   if (n_est == 0)
   {
      free(estimates);
      free(env);
      return -1;
   }

   /* Mode of the per-window estimates, counting each bin with its neighbours. */
   memset(hist, 0, sizeof(hist));
   for (b = 0; b < n_est; b++)
   {
      int bin = (int)((estimates[b] - AR_BPM_MIN) / HIST_BIN_BPM + 0.5);
      if (bin >= 0 && bin < HIST_BINS)
         hist[bin]++;
   }
   for (b = 0; b < HIST_BINS; b++)
   {
      int c = hist[b] * 2 + (b > 0 ? hist[b - 1] : 0) + (b + 1 < HIST_BINS ? hist[b + 1] : 0);
      if (c > best_count)
      {
         best_count = c;
         best_bin   = b;
      }
   }
   coarse = AR_BPM_MIN + best_bin * HIST_BIN_BPM;
   for (b = 0; b < n_est; b++)
   {
      if (fabs(estimates[b] - coarse) <= 1.0)
      {
         sum += estimates[b];
         cnt++;
      }
   }
   if (cnt)
      coarse = sum / cnt;
   out->agreeing = cnt;
   free(estimates);

   /* Refine tempo and find the phase by fitting a beat grid to the whole song. */
   for (bpm = coarse * 0.985; bpm <= coarse * 1.015; bpm += 0.01)
   {
      double period = 60.0 * erate / bpm;
      int p;
      for (p = 0; p < 48; p++)
      {
         double phase = period * p / 48.0;
         double s     = comb_score(env, env_frames, period, phase);
         if (s > best_score)
         {
            best_score  = s;
            best_period = period;
            best_phase  = phase;
         }
      }
   }
   {
      /* finer phase search around the winner */
      double step = best_period / 48.0, ph, centre = best_phase;
      for (ph = centre - step; ph <= centre + step; ph += step / 8.0)
      {
         double p = ph < 0.0 ? ph + best_period : ph;
         double s = comb_score(env, env_frames, best_period, p);
         if (s > best_score)
         {
            best_score = s;
            best_phase = p;
         }
      }
   }

   out->bpm        = 60.0 * erate / best_period;
   out->first_beat = best_phase / erate + frame_offset;
   {
      double period_sec = best_period / erate;
      while (out->first_beat >= period_sec)
         out->first_beat -= period_sec;
   }

   free(env);
   return 0;
}
