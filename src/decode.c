#include "decode.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/dr_mp3.h"
#include "../third_party/dr_flac.h"
#include "../third_party/dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

/* Raw decoder output before conversion: interleaved s16, any channel count. */
typedef struct
{
   int16_t *pcm;
   size_t   frames;
   unsigned channels;
   unsigned rate;
   void   (*release)(void *);
} raw_pcm;

static void free_mp3(void *p)  { drmp3_free(p, NULL); }
static void free_flac(void *p) { drflac_free(p, NULL); }
static void free_wav(void *p)  { drwav_free(p, NULL); }

static int try_mp3(const char *path, raw_pcm *r)
{
   drmp3_config cfg;
   drmp3_uint64 frames = 0;
   int16_t *pcm = drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &frames, NULL);
   if (!pcm || !frames)
   {
      if (pcm)
         drmp3_free(pcm, NULL);
      return -1;
   }
   r->pcm = pcm; r->frames = (size_t)frames;
   r->channels = cfg.channels; r->rate = cfg.sampleRate; r->release = free_mp3;
   return 0;
}

static int try_flac(const char *path, raw_pcm *r)
{
   unsigned ch = 0, rate = 0;
   drflac_uint64 frames = 0;
   int16_t *pcm = drflac_open_file_and_read_pcm_frames_s16(path, &ch, &rate, &frames, NULL);
   if (!pcm || !frames)
   {
      if (pcm)
         drflac_free(pcm, NULL);
      return -1;
   }
   r->pcm = pcm; r->frames = (size_t)frames;
   r->channels = ch; r->rate = rate; r->release = free_flac;
   return 0;
}

static int try_wav(const char *path, raw_pcm *r)
{
   unsigned ch = 0, rate = 0;
   drwav_uint64 frames = 0;
   int16_t *pcm = drwav_open_file_and_read_pcm_frames_s16(path, &ch, &rate, &frames, NULL);
   if (!pcm || !frames)
   {
      if (pcm)
         drwav_free(pcm, NULL);
      return -1;
   }
   r->pcm = pcm; r->frames = (size_t)frames;
   r->channels = ch; r->rate = rate; r->release = free_wav;
   return 0;
}

static int try_ogg(const char *path, raw_pcm *r)
{
   int ch = 0, rate = 0;
   short *pcm = NULL;
   int frames = stb_vorbis_decode_filename(path, &ch, &rate, &pcm);
   if (frames <= 0 || !pcm)
   {
      free(pcm);
      return -1;
   }
   r->pcm = pcm; r->frames = (size_t)frames;
   r->channels = (unsigned)ch; r->rate = (unsigned)rate; r->release = free;
   return 0;
}

static int ext_is(const char *path, const char *ext)
{
   const char *dot = strrchr(path, '.');
   size_t i;
   if (!dot)
      return 0;
   dot++;
   for (i = 0; ext[i]; i++)
      if (tolower((unsigned char)dot[i]) != ext[i])
         return 0;
   return dot[i] == '\0';
}

int ar_decode_file(const char *path, ar_audio *out)
{
   typedef int (*decoder_fn)(const char *, raw_pcm *);
   decoder_fn order[4] = { try_mp3, try_ogg, try_flac, try_wav };
   raw_pcm r;
   size_t i, out_frames;
   int16_t *dst;
   double step;

   memset(out, 0, sizeof(*out));
   memset(&r, 0, sizeof(r));

   /* Put the extension's decoder first, then fall back to the others. */
   if (ext_is(path, "ogg"))       { order[0] = try_ogg;  order[1] = try_mp3; }
   else if (ext_is(path, "flac")) { order[0] = try_flac; order[2] = try_mp3; }
   else if (ext_is(path, "wav"))  { order[0] = try_wav;  order[3] = try_mp3; }

   for (i = 0; i < 4; i++)
      if (order[i](path, &r) == 0)
         break;
   if (i == 4 || r.channels == 0 || r.rate == 0)
      return -1;

   /* Linear resample to AR_OUTPUT_RATE while mapping channels to stereo. */
   step       = (double)r.rate / AR_OUTPUT_RATE;
   out_frames = (size_t)((double)r.frames / step);
   dst        = (int16_t *)malloc(out_frames * 2 * sizeof(int16_t));
   if (!dst)
   {
      r.release(r.pcm);
      return -1;
   }

   for (i = 0; i < out_frames; i++)
   {
      double pos  = i * step;
      size_t i0   = (size_t)pos;
      size_t i1   = i0 + 1 < r.frames ? i0 + 1 : i0;
      float  frac = (float)(pos - (double)i0);
      const int16_t *a = r.pcm + i0 * r.channels;
      const int16_t *b = r.pcm + i1 * r.channels;
      float l  = a[0] + (b[0] - a[0]) * frac;
      float rr = r.channels > 1 ? a[1] + (b[1] - a[1]) * frac : l;
      dst[i * 2]     = (int16_t)l;
      dst[i * 2 + 1] = (int16_t)rr;
   }

   r.release(r.pcm);
   out->samples = dst;
   out->frames  = out_frames;
   return 0;
}

void ar_audio_free(ar_audio *a)
{
   free(a->samples);
   a->samples = NULL;
   a->frames  = 0;
}
