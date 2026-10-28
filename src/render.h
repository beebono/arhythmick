#ifndef AR_RENDER_H
#define AR_RENDER_H

#include <stdint.h>

#define AR_WIDTH  320
#define AR_HEIGHT 240

/* All drawing targets an AR_WIDTH x AR_HEIGHT XRGB8888 buffer. */
void     ar_fill(uint32_t *fb, uint32_t color);
void     ar_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color);
void     ar_disc(uint32_t *fb, int cx, int cy, int r, uint32_t color);
void     ar_ring(uint32_t *fb, int cx, int cy, int r, int thickness, uint32_t color);
void     ar_text(uint32_t *fb, int x, int y, const char *s, int scale, uint32_t color);
void     ar_text_centered(uint32_t *fb, int y, const char *s, int scale, uint32_t color);
int      ar_text_width(const char *s, int scale);
uint32_t ar_mix(uint32_t a, uint32_t b, float t);

#endif
