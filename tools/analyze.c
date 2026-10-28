/* Offline inspection of what the core would do with a song.
 *
 *   analyze <song> [subtle|wobbly|chaotic] [fail_ms]
 *   analyze --click <bpm> <seconds> <out.wav>   (writes a test click track)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/antirhythm.h"
#include "../src/bpm.h"
#include "../src/decode.h"

static void put_u32(FILE *f, uint32_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); fputc((v >> 16) & 0xff, f); fputc(v >> 24, f); }
static void put_u16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }

static int write_click(double bpm, double seconds, const char *path)
{
   size_t n = (size_t)(seconds * 44100.0), i;
   int16_t *pcm = (int16_t *)calloc(n, sizeof(int16_t));
   double t;
   int beat = 0;
   FILE *f;

   if (!pcm)
      return 1;
   for (t = 0.25; t < seconds; t += 60.0 / bpm, beat++)
   {
      size_t s = (size_t)(t * 44100.0);
      for (i = 0; i < 2000 && s + i < n; i++)
         pcm[s + i] = (int16_t)((beat % 4 ? 12000.0 : 20000.0) * exp(-(double)i / 300.0)
               * sin(2.0 * 3.14159265358979 * 1000.0 * i / 44100.0));
   }
   f = fopen(path, "wb");
   if (!f)
   {
      free(pcm);
      return 1;
   }
   fwrite("RIFF", 1, 4, f); put_u32(f, (uint32_t)(36 + n * 2));
   fwrite("WAVEfmt ", 1, 8, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 1);
   put_u32(f, 44100); put_u32(f, 44100 * 2); put_u16(f, 2); put_u16(f, 16);
   fwrite("data", 1, 4, f); put_u32(f, (uint32_t)(n * 2));
   for (i = 0; i < n; i++)
      put_u16(f, (uint16_t)pcm[i]);
   fclose(f);
   free(pcm);
   printf("wrote %s: %.1f BPM, %.0f s, first beat 0.250 s\n", path, bpm, seconds);
   return 0;
}

int main(int argc, char **argv)
{
   ar_audio  song;
   ar_tempo  tempo;
   ar_params p;
   ar_chart  c;
   int k, hits = 0, first_miss = -1, n_song;
   double fb;

   if (argc >= 5 && !strcmp(argv[1], "--click"))
      return write_click(atof(argv[2]), atof(argv[3]), argv[4]);
   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <song> [subtle|wobbly|chaotic] [fail_ms]\n"
                      "       %s --click <bpm> <seconds> <out.wav>\n", argv[0], argv[0]);
      return 2;
   }

   p.drift         = AR_DRIFT_SUBTLE;
   p.fail_window   = 0.100;
   p.near_window   = 0.050;
   p.lead_in_beats = 8;
   p.fade_in       = 3.0;
   if (argc >= 3)
      p.drift = !strcmp(argv[2], "wobbly") ? AR_DRIFT_WOBBLY
              : !strcmp(argv[2], "chaotic") ? AR_DRIFT_CHAOTIC : AR_DRIFT_SUBTLE;
   if (argc >= 4)
      p.fail_window = atof(argv[3]) / 1000.0;

   if (ar_decode_file(argv[1], &song) != 0)
   {
      fprintf(stderr, "could not decode %s\n", argv[1]);
      return 1;
   }
   if (ar_tempo_analyze(song.samples, song.frames, AR_OUTPUT_RATE, &tempo) != 0)
   {
      fprintf(stderr, "no tempo found\n");
      return 1;
   }
   ar_chart_build(&c, tempo.bpm, tempo.first_beat, (double)song.frames / AR_OUTPUT_RATE, &p, 1);

   printf("song length     %.2f s\n", c.song_length);
   printf("detected tempo  %.2f BPM (%d of %d windows agree), first beat %.3f s\n",
         tempo.bpm, tempo.agreeing, tempo.windows, tempo.first_beat);
   printf("folded period   %.4f s (%.2f BPM)\n", c.folded, 60.0 / c.folded);
   printf("pulse interval  %.4f s (%.3f BPM), detune %+.3f%%\n",
         c.interval, 60.0 / c.interval, c.sign * c.delta * 100.0);
   printf("targets         %d lead-in + %d song, audio starts at %.3f s\n",
         c.lead_in, c.n_targets - c.lead_in, c.audio_start);

   /* a player tapping exactly with the song's (folded) beat */
   n_song = c.n_targets - c.lead_in;
   fb = tempo.first_beat;
   while (fb >= c.folded)
      fb -= c.folded;
   for (k = 0; k < n_song; k++)
   {
      double t = c.audio_start + fb + k * c.folded;
      int    i = (int)floor((t - c.first_target) / c.interval + 0.5);
      if (fabs(t - ar_target_time(&c, i)) <= p.fail_window)
         hits++;
      else if (first_miss < 0)
         first_miss = k;
   }
   printf("song-follower   hits %d/%d (%.1f%%), first miss at song pulse %d (%.1f s)\n",
         hits, n_song, 100.0 * hits / n_song, first_miss + 1,
         first_miss >= 0 ? fb + first_miss * c.folded : 0.0);

   ar_audio_free(&song);
   return 0;
}
