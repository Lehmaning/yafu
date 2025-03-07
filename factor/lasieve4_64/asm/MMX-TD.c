/* MMX-TD.c
  9/30/22: Vector AVX512 code contributed by Ben Buhrow

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  You should have received a copy of the GNU General Public License along
  with this program; see the file COPYING.  If not, write to the Free
  Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
  02111-1307, USA.
*/


#include <sys/types.h>
#include <malloc.h>
#include <limits.h>

#include "siever-config.h"
#include "32bit.h"
#include "../if.h"

#include <immintrin.h>

/*
 * Auxilliary information for trial division using MMX instructions,
 * stored in the form (root0,root1,root2,root3,prime0,
 *                     prime0,prime1,prime2,prime3,
 *                     mi0,mi1,mi2,mi3),
 * where the mi are modular Inverses to the primes.
 */

static u16_t *(MMX_TdAux[2]),*(MMX_TdBound[2]);

/*
 * The projective roots used to update MMX_TdAux when the line is changed.
 */

static u16_t **(MMX_TdPr[2]);

static size_t MMX_TdAlloc[2]={0,0};

static int jps=0;

#ifndef MMX_REGW
#define MMX_REGW 4
#endif

/* Read-Ahead safety. */
#define RAS MMX_REGW

#ifdef _MSC_VER
#define AVX512_TD
// can I change MMX_REGW to 32?
// need to find everywhere this would impact...
#endif

static u16_t *
mmx_xmalloc(size_t n)
{
  u16_t *r;
  r=(u16_t*)memalign(MMX_REGW*sizeof(u16_t),n*sizeof(u16_t));
  if(r==NULL)
    complain("mmx_malloc(%Lu) failed: %m\n",(u64_t)n);
  return r;
}

void
MMX_TdAllocate(int jps_arg,size_t s0,size_t s1)
{
  int side;

  MMX_TdAlloc[0]=MMX_REGW*((s0+MMX_REGW-1)/MMX_REGW);
  MMX_TdAlloc[1]=MMX_REGW*((s1+MMX_REGW-1)/MMX_REGW);
  jps=jps_arg;

  for(side=0;side<2;side++) {
    int i;

    MMX_TdAux[side]=mmx_xmalloc(3*(MMX_TdAlloc[side]+RAS));
    MMX_TdPr[side]=xmalloc(jps*sizeof(*(MMX_TdPr[side])));
    for(i=0;i<jps;i++)
      MMX_TdPr[side][i]=mmx_xmalloc(MMX_TdAlloc[side]+RAS);
  }
}

u16_t*
MMX_TdInit(int side, u16_t* x, u16_t* x_ub, u32_t* pbound_ptr,
    int initialize)
{
    u16_t* y, * z, * u;
    u32_t p_bound;

    if (initialize == 1) {
        int i;
        if (x_ub > x + 4 * MMX_TdAlloc[side])
            Schlendrian("Buffer overflow in MMX_TdInit\n");
        z = MMX_TdAux[side] + MMX_REGW;
        y = x;
        i = 0;

#if defined(AVX512_TD)
        while (y + 4 * MMX_REGW < x_ub) {
            int k;

            __m512i xv = _mm512_load_si512(y);
            __m512i vp = _mm512_and_epi64(xv, _mm512_set1_epi64(0xffff));  // isolate primes
            __m512i vr = _mm512_srli_epi64(xv, 16);	// align roots
            __m128i vp128 = _mm512_cvtepi64_epi16(vp);
            __m128i vr128 = _mm512_cvtepi64_epi16(vr);
            __m128i vpr128 = vr128;

            //modulo32 = *y;
            //mi = *y;
            //*z = mi;
            __m512i vpi = vp;
            _mm_storeu_si128((__m128i*)z, vp128);

            //modulo32 = *y;
            //mi = 2 * mi - mi * mi * modulo32;;
            //mi = 2 * mi - mi * mi * modulo32;;
            //mi = 2 * mi - mi * mi * modulo32;;

            __m512i t2 = _mm512_mullo_epi16(vpi, vpi);
            __m512i t1 = _mm512_slli_epi16(vpi, 1);
            __m512i t3 = _mm512_mullo_epi16(t2, vp);
            vpi = _mm512_sub_epi16(t1, t3);

            t2 = _mm512_mullo_epi16(vpi, vpi);
            t1 = _mm512_slli_epi16(vpi, 1);
            t3 = _mm512_mullo_epi16(t2, vp);
            vpi = _mm512_sub_epi16(t1, t3);

            t2 = _mm512_mullo_epi16(vpi, vpi);
            t1 = _mm512_slli_epi16(vpi, 1);
            t3 = _mm512_mullo_epi16(t2, vp);
            vpi = _mm512_sub_epi16(t1, t3);

            __m128i vpi128 = _mm512_cvtepi64_epi16(vpi);
            _mm_storeu_si128((__m128i*)(z + MMX_REGW), vpi128);

            for (k = 0; k < jps; k++) {
                //MMX_TdPr[side][k][i] = rr;
                //rr = modadd32(rr, r);
                _mm_storeu_si128((__m128i *)(&MMX_TdPr[side][k][i]), vr128);
                vr128 = _mm_add_epi16(vr128, vpr128);
                __mmask8 m = _mm_cmpge_epu16_mask(vr128, vp128);
                vr128 = _mm_mask_sub_epi16(vr128, m, vr128, vp128);
            }

            y += 32, z += 24, i += 8;
        }

#else
        while (y + 4 * MMX_REGW < x_ub) {
            int j;
            for (j = 0; j < MMX_REGW; j++, y += 4, z++, i++) {
                u16_t mi;
                u32_t r, rr;
                int k;

                modulo32 = *y;
                mi = *y;
                *z = mi;
                mi = 2 * mi - mi * mi * modulo32;
                mi = 2 * mi - mi * mi * modulo32;
                mi = 2 * mi - mi * mi * modulo32;
                *(z + MMX_REGW) = mi;
                r = y[1];
                rr = r;
                for (k = 0; k < jps; k++) {
                    MMX_TdPr[side][k][i] = rr;
                    rr = modadd32(rr, r);
                }
            }
            z += 2 * MMX_REGW;
        }

#endif
    }
    z = MMX_TdAux[side];
    u = MMX_TdPr[side][jps - 1];
    p_bound = *pbound_ptr;

#if defined(AVX512_TD)

    __m128i zero = _mm_setzero_si128();
    for (y = x; y < x_ub - 4 * MMX_REGW; y = y + 4 * MMX_REGW, z += 3 * MMX_REGW, u += MMX_REGW) {

        if (z[MMX_REGW] > p_bound) break;

        __m512i xv = _mm512_load_si512(y);
        __m128i uv = _mm_loadu_si128((__m128i *)u);
        __m512i vr = _mm512_srli_epi64(xv, 48);	// align roots
        __m128i vp128 = _mm512_cvtepi64_epi16(xv);
        __m128i vr128 = _mm512_cvtepi64_epi16(vr);

        __m128i t = _mm_sub_epi16(zero, vr128);
        __mmask8 m = _mm_cmpgt_epu16_mask(vr128, zero);
        t = _mm_mask_add_epi16(t, m, t, vp128);
        _mm_storeu_si128((__m128i*)z, t);

        vr128 = _mm_add_epi16(vr128, uv);
        m = _mm_cmpge_epu16_mask(vr128, vp128);
        vr128 = _mm_mask_sub_epi16(vr128, m, vr128, vp128);

        xv = _mm512_and_epi64(xv, _mm512_set1_epi64(0xffffffffffffull));
        vr = _mm512_cvtepu16_epi64(vr128);
        vr = _mm512_slli_epi64(vr, 48);	// align roots
        xv = _mm512_or_epi64(xv, vr);

        _mm512_storeu_epi64(y, xv);
    }

#else

    for (y = x; y < x_ub - 4 * MMX_REGW; y = y + 4 * MMX_REGW, z += 3 * MMX_REGW, u += MMX_REGW) {
        int i;
        if (z[MMX_REGW] > p_bound) break;
        for (i = 0; i < MMX_REGW; i++) {
            modulo32 = y[4 * i];
            z[i] = modsub32(0, y[4 * i + 3]);
            y[4 * i + 3] = modadd32(y[4 * i + 3], u[i]);
        }
    }

#endif

    *pbound_ptr = z[-MMX_REGW - 1];
    MMX_TdBound[side] = z;
    return y;
}

void
MMX_TdUpdate(int side,int j_step)
{
#if defined(AVX512_TD)
  u16_t *x,*y;

  y=MMX_TdPr[side][j_step-1];

#if 1

  for (x = MMX_TdAux[side]; x < MMX_TdBound[side]; x += 3 * MMX_REGW, y += MMX_REGW) {

      __m128i xv = _mm_loadu_si128((__m128i*)x);
      __m128i pv = _mm_loadu_si128((__m128i*)(x + MMX_REGW));
      __m128i yv = _mm_loadu_si128((__m128i*)y);

      __m128i t = _mm_sub_epi16(xv, yv);
      __mmask8 m = _mm_cmpgt_epu16_mask(yv, xv);
      t = _mm_mask_add_epi16(t, m, t, pv);
      _mm_storeu_si128((__m128i*)x, t);
  }

#else
  for (x = MMX_TdAux[side]; x < MMX_TdBound[side]; x += 3 * MMX_REGW, y += MMX_REGW) {
      int i;
      for (i = 0; i < MMX_REGW; i++) {
          modulo32 = x[MMX_REGW + i];
          x[i] = modsub32(x[i], y[i]);
      }
  }

#endif

#else
#if MMX_REGW == 4
  asm_TdUpdate4(MMX_TdAux[side],MMX_TdBound[side],MMX_TdPr[side][j_step-1]);
#elif MMX_REGW == 8
  asm_TdUpdate8(MMX_TdAux[side],MMX_TdBound[side],MMX_TdPr[side][j_step-1]);
#else
#error "No asm for this MMX_REGW"
#endif
#endif
}

u32_t *asm_MMX_Td4(u32_t*,u32_t,u16_t*,u16_t*);
u32_t *asm_MMX_Td8(u32_t*,u32_t,u16_t*,u16_t*);

#ifdef MMX_TDBENCH
u64_t MMX_TdNloop=0;
#endif

u32_t *
MMX_Td(u32_t* pbuf, int side, u16_t strip_i)
{
#if !defined(AVX512_TD)
#ifdef MMX_TDBENCH
    MMX_TdNloop += (MMX_TdBound[side] - MMX_TdAux[side]) / MMX_REGW;
#endif
#if MMX_REGW == 4
    return asm_MMX_Td4(pbuf, strip_i, MMX_TdAux[side], MMX_TdBound[side]);
#elif MMX_REGW == 8
    return asm_MMX_Td8(pbuf, strip_i, MMX_TdAux[side], MMX_TdBound[side]);
#else
#error "No asm for this MMX_REGW"
#endif
#else
    u16_t* x;

#if defined(AVX512_TD)

    __m128i vs = _mm_set1_epi16(strip_i);
    __m128i zero = _mm_setzero_si128();
    for (x = MMX_TdAux[side]; x < MMX_TdBound[side]; x += 3 * MMX_REGW) {

        __m128i x0 = _mm_loadu_si128((__m128i*)(&(x[0])));
        __m128i x1 = _mm_loadu_si128((__m128i*)(&(x[MMX_REGW])));
        __m128i x2 = _mm_loadu_si128((__m128i*)(&(x[2 * MMX_REGW])));

        __m128i tv = _mm_add_epi16(vs, x0);
        __m128i tv2 = _mm_mullo_epi16(tv, x2);
        __m128i tv3 = _mm_mulhi_epu16(tv2, x1);

        __mmask8 m = _mm_cmpeq_epu16_mask(tv3, zero);

        while (m > 0)
        {
            int id = _tzcnt_u32(m);
            *(pbuf++) = x[MMX_REGW + id];
            m = _blsr_u32(m);
        }

    }

    return pbuf;

#else

    for (x = MMX_TdAux[side]; x < MMX_TdBound[side]; x += 2 * MMX_REGW) {
        int i;

        for (i = 0; i < MMX_REGW; i++, x++) {
            u16_t t;

            modulo32 = x[MMX_REGW];

            t = strip_i + x[0];
            t *= x[2 * MMX_REGW];

            if (((modulo32 * (u32_t)t) & 0xffff0000) == 0)
                *(pbuf++) = modulo32;
        }
    }
    return pbuf;

#endif

#endif
}
