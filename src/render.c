#include "render.h"

#include <string.h>

#include "../third_party/font8x8_basic.h"

static void put(uint32_t *fb, int x, int y, uint32_t c)
{
   if (x >= 0 && y >= 0 && x < AR_WIDTH && y < AR_HEIGHT)
      fb[y * AR_WIDTH + x] = c;
}

void ar_fill(uint32_t *fb, uint32_t color)
{
   int i;
   for (i = 0; i < AR_WIDTH * AR_HEIGHT; i++)
      fb[i] = color;
}

void ar_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color)
{
   int i, j;
   for (j = y; j < y + h; j++)
      for (i = x; i < x + w; i++)
         put(fb, i, j, color);
}

void ar_disc(uint32_t *fb, int cx, int cy, int r, uint32_t color)
{
   int x, y;
   for (y = -r; y <= r; y++)
      for (x = -r; x <= r; x++)
         if (x * x + y * y <= r * r)
            put(fb, cx + x, cy + y, color);
}

void ar_ring(uint32_t *fb, int cx, int cy, int r, int thickness, uint32_t color)
{
   int x, y, inner = r - thickness;
   if (inner < 0)
      inner = 0;
   for (y = -r; y <= r; y++)
      for (x = -r; x <= r; x++)
      {
         int d = x * x + y * y;
         if (d <= r * r && d > inner * inner)
            put(fb, cx + x, cy + y, color);
      }
}

int ar_text_width(const char *s, int scale)
{
   return (int)strlen(s) * 8 * scale;
}

void ar_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t color)
{
   for (; *s; s++, x += 8 * scale)
   {
      const char *glyph = font8x8_basic[(unsigned char)*s & 0x7f];
      int row, col;
      for (row = 0; row < 8; row++)
         for (col = 0; col < 8; col++)
            if (glyph[row] & (1 << col))
               ar_rect(fb, x + col * scale, y + row * scale, scale, scale, color);
   }
}

void ar_text_centered(uint32_t *fb, int y, const char *s, int scale, uint32_t color)
{
   ar_text(fb, (AR_WIDTH - ar_text_width(s, scale)) / 2, y, s, scale, color);
}

uint32_t ar_mix(uint32_t a, uint32_t b, float t)
{
   uint32_t out = 0;
   int shift;
   if (t <= 0.0f) return a;
   if (t >= 1.0f) return b;
   for (shift = 0; shift <= 16; shift += 8)
   {
      float ca = (float)((a >> shift) & 0xff);
      float cb = (float)((b >> shift) & 0xff);
      out |= (uint32_t)(ca + (cb - ca) * t) << shift;
   }
   return out;
}
