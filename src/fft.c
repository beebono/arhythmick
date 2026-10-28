#include "fft.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void ar_fft(float *re, float *im, int n)
{
   int i, j, len;

   /* bit-reversal permutation */
   for (i = 1, j = 0; i < n; i++)
   {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
         j ^= bit;
      j ^= bit;
      if (i < j)
      {
         float t = re[i]; re[i] = re[j]; re[j] = t;
         t = im[i]; im[i] = im[j]; im[j] = t;
      }
   }

   for (len = 2; len <= n; len <<= 1)
   {
      double ang = -2.0 * M_PI / len;
      float wr = (float)cos(ang), wi = (float)sin(ang);
      for (i = 0; i < n; i += len)
      {
         float cr = 1.0f, ci = 0.0f;
         int k;
         for (k = 0; k < len / 2; k++)
         {
            int a = i + k, b = i + k + len / 2;
            float xr = re[b] * cr - im[b] * ci;
            float xi = re[b] * ci + im[b] * cr;
            float ncr;
            re[b] = re[a] - xr; im[b] = im[a] - xi;
            re[a] += xr;        im[a] += xi;
            ncr = cr * wr - ci * wi;
            ci  = cr * wi + ci * wr;
            cr  = ncr;
         }
      }
   }
}
