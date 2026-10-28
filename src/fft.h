#ifndef AR_FFT_H
#define AR_FFT_H

/* In-place iterative radix-2 complex FFT. n must be a power of two. */
void ar_fft(float *re, float *im, int n);

#endif
