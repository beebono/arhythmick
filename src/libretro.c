#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libretro.h"
#include "antirhythm.h"
#include "bpm.h"
#include "decode.h"
#include "render.h"

#define FPS              60
#define FRAMES_PER_RUN   (AR_OUTPUT_RATE / FPS)
#define FAIL_FADE_SEC    0.4
#define BOUNCE_SEC       0.18
#define TINT_SEC         0.30
#define ZONE_R           44   /* radius of the hit zone centre line */
#define RING_TRAVEL      70.0 /* pixels a ring travels in one interval */
#define BUTTON_R         20
#define BUTTON_BOUNCE    9
#define GRADE_SHOW_SEC   0.5

#define COL_BG       0x0d0d1aU
#define COL_BG_FAIL  0x2a0a10U
#define COL_BG_CLEAR 0x0a2416U
#define COL_DIM      0x4a4a6aU
#define COL_TEXT     0xe8e8f0U
#define COL_PULSE    0xff3d7fU
#define COL_RING     0x7fd4ffU
#define COL_ZONE     0x1d2a44U
#define COL_ZONE_EDGE 0x3c5a8aU
#define COL_BUTTON   0x5a2440U
#define COL_PERFECT  0x7fff9aU
#define COL_NEAR     0xffd24aU
#define COL_BAD      0xff4a4aU

typedef enum { SCREEN_TITLE, SCREEN_RUN } screen;

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static uint32_t framebuffer[AR_WIDTH * AR_HEIGHT];
static uint16_t framebuffer565[AR_WIDTH * AR_HEIGHT];
static int      use_rgb565;
static int16_t  audio_out[FRAMES_PER_RUN * 2];

static ar_audio  song;
static ar_tempo  tempo;
static ar_params params;
static ar_chart  chart;
static ar_judge  judge;
static double    latency;      /* seconds, positive = player/output is late */
static uint32_t  song_seed;
static char      song_name[40];

static screen  current_screen;
static int64_t clock_frames;   /* audio frames since the lead-in started */
static double  fail_time;      /* game time when the run ended in failure */
static int     prev_buttons;
static double  press_time;     /* raw game time of the last press, -1 if none */
static ar_grade press_grade;

/* ---------------------------------------------------------------- logging */

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

static void show_message(const char *msg)
{
   struct retro_message m;
   m.msg    = msg;
   m.frames = 240;
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

/* ---------------------------------------------------------------- options */

static struct retro_core_option_v2_definition option_defs[] = {
   {
      "arhythmick_drift", "Drift", NULL,
      "How far the pulse is detuned from the song. Subtle: the music starts in sync and slides steadily "
      "to half a beat off by the end. Wobbly: it slides out of phase and back twice. Chaotic: five times.",
      NULL, NULL,
      { { "subtle", "Subtle" }, { "wobbly", "Wobbly" }, { "chaotic", "Chaotic" }, { NULL, NULL } },
      "subtle"
   },
   {
      "arhythmick_fail_window", "Fail Window", NULL,
      "A tap further than this from the pulse fails the song.",
      NULL, NULL,
      { { "80", "80 ms" }, { "100", "100 ms" }, { "120", "120 ms" }, { "150", "150 ms" }, { NULL, NULL } },
      "100"
   },
   {
      "arhythmick_near_window", "Perfect Window", NULL,
      "Taps within this are Perfect; between this and the fail window they are Near (still allowed).",
      NULL, NULL,
      { { "30", "30 ms" }, { "40", "40 ms" }, { "50", "50 ms" }, { "60", "60 ms" }, { NULL, NULL } },
      "60"
   },
   {
      "arhythmick_lead_in", "Lead-in Beats", NULL,
      "Pulses shown before the song fades in.",
      NULL, NULL,
      { { "4", NULL }, { "8", NULL }, { "16", NULL }, { NULL, NULL } },
      "4"
   },
   {
      "arhythmick_latency", "Latency Offset", NULL,
      "Raise this if your taps register late (Bluetooth controllers, slow displays).",
      NULL, NULL,
      {
         { "-100", "-100 ms" }, { "-80", "-80 ms" }, { "-60", "-60 ms" }, { "-40", "-40 ms" }, { "-20", "-20 ms" },
         { "0", "0 ms" }, { "20", "20 ms" }, { "40", "40 ms" }, { "60", "60 ms" }, { "80", "80 ms" },
         { "100", "100 ms" }, { "120", "120 ms" }, { "150", "150 ms" }, { "200", "200 ms" }, { NULL, NULL }
      },
      "0"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static void set_core_options(void)
{
   static struct retro_core_options_v2 opts = { NULL, option_defs };
   static struct retro_variable legacy[sizeof(option_defs) / sizeof(option_defs[0])];
   static char legacy_text[sizeof(option_defs) / sizeof(option_defs[0])][512];
   unsigned version = 0;
   size_t i;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2
         && environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts))
      return;

   /* Legacy frontends: "Description; default|other|..." */
   for (i = 0; option_defs[i].key; i++)
   {
      const struct retro_core_option_v2_definition *d = &option_defs[i];
      char *out = legacy_text[i];
      size_t v;
      int n = snprintf(out, 512, "%s; %s", d->desc, d->default_value);
      for (v = 0; d->values[v].value && n < 500; v++)
         if (strcmp(d->values[v].value, d->default_value) != 0)
            n += snprintf(out + n, 512 - n, "|%s", d->values[v].value);
      legacy[i].key   = d->key;
      legacy[i].value = out;
   }
   legacy[i].key   = NULL;
   legacy[i].value = NULL;
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, legacy);
}

static const char *get_option(const char *key, const char *fallback)
{
   struct retro_variable var;
   var.key   = key;
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return var.value;
   return fallback;
}

static void read_options(void)
{
   const char *drift = get_option("arhythmick_drift", "subtle");
   params.drift = !strcmp(drift, "wobbly") ? AR_DRIFT_WOBBLY
                : !strcmp(drift, "chaotic") ? AR_DRIFT_CHAOTIC : AR_DRIFT_SUBTLE;
   params.fail_window   = atoi(get_option("arhythmick_fail_window", "100")) / 1000.0;
   params.near_window   = atoi(get_option("arhythmick_near_window", "50")) / 1000.0;
   params.lead_in_beats = atoi(get_option("arhythmick_lead_in", "8"));
   params.fade_in       = 3.0;
   latency              = atoi(get_option("arhythmick_latency", "0")) / 1000.0;
   if (params.near_window > params.fail_window)
      params.near_window = params.fail_window;
}

/* ---------------------------------------------------------------- game flow */

static double game_time(void)
{
   return (double)clock_frames / AR_OUTPUT_RATE;
}

static void start_run(void)
{
   read_options();
   ar_chart_build(&chart, tempo.bpm, tempo.first_beat,
         (double)song.frames / AR_OUTPUT_RATE, &params, song_seed);
   ar_judge_reset(&judge);
   clock_frames   = 0;
   fail_time      = -1.0;
   press_time     = -1.0;
   press_grade    = AR_GRADE_NONE;
   current_screen = SCREEN_RUN;
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[arhythmick] song %.2f BPM, pulse %.3f s (%.2f BPM, %+.3f%%), %d targets\n",
            chart.song_bpm, chart.interval, 60.0 / chart.interval,
            chart.sign * chart.delta * 100.0, chart.n_targets);
}

static int pressed(int buttons, int id)
{
   return (buttons & (1 << id)) && !(prev_buttons & (1 << id));
}

static int read_buttons(void)
{
   static const int ids[] = {
      RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_SELECT
   };
   int buttons = 0;
   size_t i;
   for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, ids[i]))
         buttons |= 1 << ids[i];
   return buttons;
}

static void handle_input(void)
{
   int buttons, tap;

   input_poll_cb();
   buttons = read_buttons();
   tap = pressed(buttons, RETRO_DEVICE_ID_JOYPAD_A) || pressed(buttons, RETRO_DEVICE_ID_JOYPAD_B)
      || pressed(buttons, RETRO_DEVICE_ID_JOYPAD_X) || pressed(buttons, RETRO_DEVICE_ID_JOYPAD_Y);

   if (current_screen == SCREEN_TITLE)
   {
      if (tap || pressed(buttons, RETRO_DEVICE_ID_JOYPAD_START))
         start_run();
   }
   else if (judge.status == AR_PLAYING)
   {
      if (tap)
      {
         ar_judge_tap(&judge, &chart, &params, game_time() - latency);
         press_time  = game_time();
         press_grade = judge.status == AR_FAILED ? AR_GRADE_NONE : judge.last_grade;
      }
      if (judge.status == AR_FAILED)
         fail_time = game_time();
   }
   else if (pressed(buttons, RETRO_DEVICE_ID_JOYPAD_START))
      start_run();

   if (pressed(buttons, RETRO_DEVICE_ID_JOYPAD_SELECT))
      current_screen = SCREEN_TITLE;

   prev_buttons = buttons;
}

static void mix_audio(void)
{
   int i;
   memset(audio_out, 0, sizeof(audio_out));

   if (current_screen == SCREEN_RUN)
   {
      int64_t start = (int64_t)(chart.audio_start * AR_OUTPUT_RATE);
      for (i = 0; i < FRAMES_PER_RUN; i++)
      {
         int64_t f   = clock_frames + i;
         int64_t pos = f - start;
         double  t, gain;
         if (pos < 0 || pos >= (int64_t)song.frames)
            continue;
         t    = (double)f / AR_OUTPUT_RATE;
         gain = (double)pos / AR_OUTPUT_RATE / params.fade_in;
         gain = gain >= 1.0 ? 1.0 : gain * gain;
         if (fail_time >= 0.0)
         {
            double out = 1.0 - (t - fail_time) / FAIL_FADE_SEC;
            if (out <= 0.0)
               continue;
            gain *= out;
         }
         audio_out[i * 2]     = (int16_t)(song.samples[pos * 2] * gain);
         audio_out[i * 2 + 1] = (int16_t)(song.samples[pos * 2 + 1] * gain);
      }
      clock_frames += FRAMES_PER_RUN;
   }

   audio_batch_cb(audio_out, FRAMES_PER_RUN);
}

/* ---------------------------------------------------------------- drawing */

static void draw_title(void)
{
   char line[64];
   ar_fill(framebuffer, COL_BG);
   ar_text_centered(framebuffer, 36, "ARHYTHMICK", 3, COL_PULSE);
   ar_text_centered(framebuffer, 70, "DON'T FOLLOW THE MUSIC.", 1, COL_TEXT);
   ar_text_centered(framebuffer, 82, "FOLLOW THE PULSE.", 1, COL_TEXT);
   ar_text_centered(framebuffer, 118, song_name, 1, COL_RING);
   snprintf(line, sizeof(line), "SONG %.1f BPM", tempo.bpm);
   ar_text_centered(framebuffer, 134, line, 1, COL_DIM);
   ar_text_centered(framebuffer, 170, "A: START   SELECT: TITLE", 1, COL_TEXT);
   ar_text_centered(framebuffer, 182, "TAP A/B/X/Y ON EVERY PULSE", 1, COL_DIM);
}

static uint32_t grade_color(ar_grade g)
{
   switch (g)
   {
      case AR_GRADE_PERFECT:       return COL_PERFECT;
      case AR_GRADE_NEAR:          return COL_NEAR;
      case AR_GRADE_PRACTICE_MISS: return COL_DIM;
      default:                     return COL_PULSE;
   }
}

static void draw_run(void)
{
   const int cx = AR_WIDTH / 2, cy = 112;
   double t = game_time(); /* visuals run on the raw clock; latency only shifts judging */
   int last = (int)floor((t - chart.first_target) / chart.interval);
   uint32_t bg = COL_BG;
   char line[64];

   if (judge.status == AR_FAILED)
      bg = COL_BG_FAIL;
   else if (judge.status == AR_CLEARED)
      bg = COL_BG_CLEAR;
   ar_fill(framebuffer, bg);

   /* HUD */
   snprintf(line, sizeof(line), "SONG %.1f", chart.song_bpm);
   ar_text(framebuffer, 8, 8, line, 1, COL_DIM);

   if (judge.status == AR_PLAYING)
   {
      /* Rings shrink at a constant speed and cross the zone's centre exactly
       * on a pulse; the zone is as wide as the Perfect window. */
      double speed = RING_TRAVEL / chart.interval; /* pixels per second */
      int    half  = (int)(speed * params.near_window + 0.5);
      int    k;
      double age   = t - press_time;
      float  bump  = press_time >= 0.0 && age < BOUNCE_SEC ? (float)(1.0 - age / BOUNCE_SEC) : 0.0f;
      float  tint  = press_time >= 0.0 && age < TINT_SEC ? (float)(1.0 - age / TINT_SEC) : 0.0f;

      if (half < 1)
         half = 1;
      ar_ring(framebuffer, cx, cy, ZONE_R + half, 2 * half + 1, COL_ZONE);
      ar_ring(framebuffer, cx, cy, ZONE_R + half, 1, COL_ZONE_EDGE);
      ar_ring(framebuffer, cx, cy, ZONE_R - half, 1, COL_ZONE_EDGE);

      for (k = last; k <= last + 1; k++)
      {
         double dt;
         int r;
         if (k < 0 || k >= chart.n_targets || k < judge.next - 1)
            continue;
         dt = ar_target_time(&chart, k) - t;
         if (dt > chart.interval || dt < -params.near_window * 2.0)
            continue;
         if (k < judge.next && dt < 0.0)
            continue; /* already hit: let it vanish */
         r = ZONE_R + (int)(speed * dt + 0.5);
         ar_ring(framebuffer, cx, cy, r, 3, COL_RING);
      }

      /* the button: bounces with the player's press, not with the pulse */
      ar_disc(framebuffer, cx, cy, BUTTON_R + (int)(BUTTON_BOUNCE * bump * bump),
            ar_mix(COL_BUTTON, grade_color(press_grade), tint));

      if (judge.next < chart.lead_in || t < chart.audio_start)
      {
         int left = chart.lead_in - (last + 1);
         ar_text_centered(framebuffer, 28, "TAP WITH THE PULSE", 1, COL_TEXT);
         if (left > 0)
         {
            snprintf(line, sizeof(line), "%d", left);
            ar_text_centered(framebuffer, 40, line, 2, COL_RING);
         }
      }
   }

   if (judge.last_tap >= 0.0 && t - latency - judge.last_tap < GRADE_SHOW_SEC && judge.status == AR_PLAYING)
   {
      int ms = (int)floor(judge.last_error * 1000.0 + 0.5);
      if (judge.last_grade == AR_GRADE_PERFECT)
         ar_text_centered(framebuffer, 158, "PERFECT", 2, COL_PERFECT);
      else if (judge.last_grade == AR_GRADE_NEAR)
      {
         snprintf(line, sizeof(line), "NEAR %+dMS", ms);
         ar_text_centered(framebuffer, 158, line, 2, COL_NEAR);
      }
      else if (judge.last_grade == AR_GRADE_PRACTICE_MISS)
      {
         snprintf(line, sizeof(line), "OFF %+dMS", ms);
         ar_text_centered(framebuffer, 158, line, 2, COL_DIM);
      }
   }

   if (judge.status == AR_FAILED)
   {
      const char *why = judge.fail_reason == AR_FAIL_SKIPPED ? "YOU SKIPPED A PULSE"
                      : judge.fail_reason == AR_FAIL_EXTRA   ? "EXTRA TAP"
                      : "TOO FAR OFF THE PULSE";
      ar_text_centered(framebuffer, 70, "FAILED", 4, COL_BAD);
      ar_text_centered(framebuffer, 120, why, 1, COL_TEXT);
      if (judge.fail_reason == AR_FAIL_MISS || judge.fail_reason == AR_FAIL_EXTRA)
      {
         snprintf(line, sizeof(line), "%+d MS", (int)floor(judge.last_error * 1000.0 + 0.5));
         ar_text_centered(framebuffer, 134, line, 1, COL_NEAR);
      }
      snprintf(line, sizeof(line), "PULSE %d OF %d", judge.fail_target - chart.lead_in + 1,
            chart.n_targets - chart.lead_in);
      ar_text_centered(framebuffer, 148, line, 1, COL_DIM);
      ar_text_centered(framebuffer, 180, "START: RETRY", 1, COL_TEXT);
   }
   else if (judge.status == AR_CLEARED)
   {
      ar_text_centered(framebuffer, 70, "CLEARED", 4, COL_PERFECT);
      snprintf(line, sizeof(line), "PERFECT %d   NEAR %d", judge.perfect, judge.near);
      ar_text_centered(framebuffer, 124, line, 1, COL_TEXT);
      snprintf(line, sizeof(line), "PULSE %.2f BPM VS SONG %.1f", 60.0 / chart.interval, chart.song_bpm);
      ar_text_centered(framebuffer, 140, line, 1, COL_DIM);
      ar_text_centered(framebuffer, 180, "START: AGAIN", 1, COL_TEXT);
   }
   else
   {
      snprintf(line, sizeof(line), "COMBO %d", judge.combo);
      ar_text(framebuffer, 8, AR_HEIGHT - 26, line, 1, COL_TEXT);
   }

   /* progress through the song */
   {
      double song_t = game_time() - chart.audio_start;
      double frac   = song_t / chart.song_length;
      if (frac < 0.0) frac = 0.0;
      if (frac > 1.0) frac = 1.0;
      ar_rect(framebuffer, 0, AR_HEIGHT - 6, AR_WIDTH, 6, 0x1c1c30U);
      ar_rect(framebuffer, 0, AR_HEIGHT - 6, (int)(AR_WIDTH * frac), 6, COL_RING);
   }
}

static void present(void)
{
   if (use_rgb565)
   {
      int i;
      for (i = 0; i < AR_WIDTH * AR_HEIGHT; i++)
      {
         uint32_t c = framebuffer[i];
         framebuffer565[i] = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
      }
      video_cb(framebuffer565, AR_WIDTH, AR_HEIGHT, AR_WIDTH * sizeof(uint16_t));
   }
   else
      video_cb(framebuffer, AR_WIDTH, AR_HEIGHT, AR_WIDTH * sizeof(uint32_t));
}

/* ---------------------------------------------------------------- libretro API */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool no_game = false;

   environ_cb = cb;
   log_cb     = cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) ? logging.log : fallback_log;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   set_core_options();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)         { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)           { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)               { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)             { input_state_cb = cb; }

RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Arhythmick";
   info->library_version  = "0.1.0";
   info->valid_extensions = "mp3|ogg|flac|wav";
   info->need_fullpath    = true;
   info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = AR_WIDTH;
   info->geometry.base_height  = AR_HEIGHT;
   info->geometry.max_width    = AR_WIDTH;
   info->geometry.max_height   = AR_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = FPS;
   info->timing.sample_rate    = AR_OUTPUT_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port; (void)device;
}

RETRO_API void retro_reset(void)
{
   current_screen = SCREEN_TITLE;
}

RETRO_API void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      read_options(); /* latency applies now, the rest on the next run */

   handle_input();
   if (current_screen == SCREEN_RUN)
   {
      ar_judge_advance(&judge, &chart, &params, game_time() - latency);
      if (judge.status == AR_FAILED && fail_time < 0.0)
         fail_time = game_time();
   }
   mix_audio();

   if (current_screen == SCREEN_TITLE)
      draw_title();
   else
      draw_run();
   present();
}

static uint32_t hash_name(const char *s, size_t frames)
{
   uint32_t h = 2166136261u;
   for (; *s; s++)
      h = (h ^ (unsigned char)*s) * 16777619u;
   return (h ^ (uint32_t)frames) * 16777619u;
}

static void set_song_name(const char *path)
{
   const char *base = strrchr(path, '/'), *alt = strrchr(path, '\\');
   size_t i;
   if (alt > base)
      base = alt;
   base = base ? base + 1 : path;
   for (i = 0; base[i] && i < sizeof(song_name) - 1; i++)
   {
      char c = base[i];
      song_name[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
   }
   song_name[i] = '\0';
   if (i == sizeof(song_name) - 1)
      memcpy(song_name + i - 3, "...", 3);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Tap" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Tap" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Tap" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Tap" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start / Retry" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Title" },
      { 0, 0, 0, 0, NULL },
   };

   if (!game || !game->path)
      return false;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      fmt = RETRO_PIXEL_FORMAT_RGB565;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
         return false;
      use_rgb565 = 1;
   }
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   if (ar_decode_file(game->path, &song) != 0)
   {
      log_cb(RETRO_LOG_ERROR, "[arhythmick] could not decode %s\n", game->path);
      show_message("Arhythmick: could not decode this file");
      return false;
   }
   if (ar_tempo_analyze(song.samples, song.frames, AR_OUTPUT_RATE, &tempo) != 0)
   {
      log_cb(RETRO_LOG_ERROR, "[arhythmick] no tempo found in %s\n", game->path);
      show_message("Arhythmick: could not find a beat in this song");
      ar_audio_free(&song);
      return false;
   }
   log_cb(RETRO_LOG_INFO, "[arhythmick] %s: %.2f BPM (%d/%d windows), first beat %.3f s\n",
         game->path, tempo.bpm, tempo.agreeing, tempo.windows, tempo.first_beat);

   song_seed = hash_name(game->path, song.frames);
   set_song_name(game->path);
   read_options();
   current_screen = SCREEN_TITLE;
   prev_buttons   = 0;
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type; (void)info; (void)num;
   return false;
}

RETRO_API void retro_unload_game(void)
{
   ar_audio_free(&song);
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index; (void)enabled; (void)code;
}
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
