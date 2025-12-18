#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ultra64.h>

#ifdef __SSE4_1__
#include <immintrin.h>
#define HAS_SSE41 1
#define HAS_NEON 0
#elif __ARM_NEON
#include <arm_neon.h>
#define HAS_SSE41 0
#define HAS_NEON 1
#else
#define HAS_SSE41 0
#define HAS_NEON 0
#endif

//#pragma GCC optimize ("unroll-loops")
#define MEM_BARRIER() asm volatile("" : : : "memory");
#define MEM_BARRIER_PREF(ptr) asm volatile("pref @%0" : : "r"((ptr)) : "memory")

#if HAS_SSE41
#define LOADLH(l, h) _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd((const double *)(l)), (const double *)(h)))
#endif

void n64_memcpy(void* dst, const void* src, size_t size);

#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)

#define BUF_U8(a) (rspa.buf.as_u8 + (a))
#define BUF_S16(a) (rspa.buf.as_s16 + (a) / sizeof(int16_t))

static struct  __attribute__((aligned(32)))  {
    union  __attribute__((aligned(32))) {
        int16_t __attribute__((aligned(32))) as_s16[2512 / sizeof(int16_t)];
        uint8_t __attribute__((aligned(32))) as_u8[2512];
    } buf;
    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    int16_t vol[2];

    uint16_t dry_right;
    uint16_t wet_left;
    uint16_t wet_right;

    int16_t target[2];
    int32_t rate[2];

    int16_t vol_dry;
    int16_t vol_wet;

    ADPCM_STATE *adpcm_loop_state;

    float __attribute__((aligned(32))) adpcm_table[8][2][8];
/*     union {
        int16_t as_s16[2512 / sizeof(int16_t)];
        uint8_t as_u8[2512];
    } buf; */
} rspa;
static float __attribute__((aligned(32))) resample_table[64][4] = {
    {
        (f32) 3129,
        (f32) 26285,
        (f32) 3398,
        (f32) -33,
    },
    {
        (f32) 2873,
        (f32) 26262,
        (f32) 3679,
        (f32) -40,
    },
    {
        (f32) 2628,
        (f32) 26217,
        (f32) 3971,
        (f32) -48,
    },
    {
        (f32) 2394,
        (f32) 26150,
        (f32) 4276,
        (f32) -56,
    },
    {
        (f32) 2173,
        (f32) 26061,
        (f32) 4592,
        (f32) -65,
    },
    {
        (f32) 1963,
        (f32) 25950,
        (f32) 4920,
        (f32) -74,
    },
    {
        (f32) 1764,
        (f32) 25817,
        (f32) 5260,
        (f32) -84,
    },
    {
        (f32) 1576,
        (f32) 25663,
        (f32) 5611,
        (f32) -95,
    },
    {
        (f32) 1399,
        (f32) 25487,
        (f32) 5974,
        (f32) -106,
    },
    {
        (f32) 1233,
        (f32) 25291,
        (f32) 6347,
        (f32) -118,
    },
    {
        (f32) 1077,
        (f32) 25075,
        (f32) 6732,
        (f32) -130,
    },
    {
        (f32) 932,
        (f32) 24838,
        (f32) 7127,
        (f32) -143,
    },
    {
        (f32) 796,
        (f32) 24583,
        (f32) 7532,
        (f32) -156,
    },
    {
        (f32) 671,
        (f32) 24309,
        (f32) 7947,
        (f32) -170,
    },
    {
        (f32) 554,
        (f32) 24016,
        (f32) 8371,
        (f32) -184,
    },
    {
        (f32) 446,
        (f32) 23706,
        (f32) 8804,
        (f32) -198,
    },
    {
        (f32) 347,
        (f32) 23379,
        (f32) 9246,
        (f32) -212,
    },
    {
        (f32) 257,
        (f32) 23036,
        (f32) 9696,
        (f32) -226,
    },
    {
        (f32) 174,
        (f32) 22678,
        (f32) 10153,
        (f32) -240,
    },
    {
        (f32) 99,
        (f32) 22304,
        (f32) 10618,
        (f32) -254,
    },
    {
        (f32) 31,
        (f32) 21917,
        (f32) 11088,
        (f32) -268,
    },
    {
        (f32) -30,
        (f32) 21517,
        (f32) 11564,
        (f32) -280,
    },
    {
        (f32) -84,
        (f32) 21104,
        (f32) 12045,
        (f32) -293,
    },
    {
        (f32) -132,
        (f32) 20679,
        (f32) 12531,
        (f32) -304,
    },
    {
        (f32) -173,
        (f32) 20244,
        (f32) 13020,
        (f32) -314,
    },
    {
        (f32) -210,
        (f32) 19799,
        (f32) 13512,
        (f32) -323,
    },
    {
        (f32) -241,
        (f32) 19345,
        (f32) 14006,
        (f32) -330,
    },
    {
        (f32) -267,
        (f32) 18882,
        (f32) 14501,
        (f32) -336,
    },
    {
        (f32) -289,
        (f32) 18413,
        (f32) 14997,
        (f32) -340,
    },
    {
        (f32) -306,
        (f32) 17937,
        (f32) 15493,
        (f32) -341,
    },
    {
        (f32) -320,
        (f32) 17456,
        (f32) 15988,
        (f32) -340,
    },
    {
        (f32) -330,
        (f32) 16970,
        (f32) 16480,
        (f32) -337,
    },
    {
        (f32) -337,
        (f32) 16480,
        (f32) 16970,
        (f32) -330,
    },
    {
        (f32) -340,
        (f32) 15988,
        (f32) 17456,
        (f32) -320,
    },
    {
        (f32) -341,
        (f32) 15493,
        (f32) 17937,
        (f32) -306,
    },
    {
        (f32) -340,
        (f32) 14997,
        (f32) 18413,
        (f32) -289,
    },
    {
        (f32) -336,
        (f32) 14501,
        (f32) 18882,
        (f32) -267,
    },
    {
        (f32) -330,
        (f32) 14006,
        (f32) 19345,
        (f32) -241,
    },
    {
        (f32) -323,
        (f32) 13512,
        (f32) 19799,
        (f32) -210,
    },
    {
        (f32) -314,
        (f32) 13020,
        (f32) 20244,
        (f32) -173,
    },
    {
        (f32) -304,
        (f32) 12531,
        (f32) 20679,
        (f32) -132,
    },
    {
        (f32) -293,
        (f32) 12045,
        (f32) 21104,
        (f32) -84,
    },
    {
        (f32) -280,
        (f32) 11564,
        (f32) 21517,
        (f32) -30,
    },
    {
        (f32) -268,
        (f32) 11088,
        (f32) 21917,
        (f32) 31,
    },
    {
        (f32) -254,
        (f32) 10618,
        (f32) 22304,
        (f32) 99,
    },
    {
        (f32) -240,
        (f32) 10153,
        (f32) 22678,
        (f32) 174,
    },
    {
        (f32) -226,
        (f32) 9696,
        (f32) 23036,
        (f32) 257,
    },
    {
        (f32) -212,
        (f32) 9246,
        (f32) 23379,
        (f32) 347,
    },
    {
        (f32) -198,
        (f32) 8804,
        (f32) 23706,
        (f32) 446,
    },
    {
        (f32) -184,
        (f32) 8371,
        (f32) 24016,
        (f32) 554,
    },
    {
        (f32) -170,
        (f32) 7947,
        (f32) 24309,
        (f32) 671,
    },
    {
        (f32) -156,
        (f32) 7532,
        (f32) 24583,
        (f32) 796,
    },
    {
        (f32) -143,
        (f32) 7127,
        (f32) 24838,
        (f32) 932,
    },
    {
        (f32) -130,
        (f32) 6732,
        (f32) 25075,
        (f32) 1077,
    },
    {
        (f32) -118,
        (f32) 6347,
        (f32) 25291,
        (f32) 1233,
    },
    {
        (f32) -106,
        (f32) 5974,
        (f32) 25487,
        (f32) 1399,
    },
    {
        (f32) -95,
        (f32) 5611,
        (f32) 25663,
        (f32) 1576,
    },
    {
        (f32) -84,
        (f32) 5260,
        (f32) 25817,
        (f32) 1764,
    },
    {
        (f32) -74,
        (f32) 4920,
        (f32) 25950,
        (f32) 1963,
    },
    {
        (f32) -65,
        (f32) 4592,
        (f32) 26061,
        (f32) 2173,
    },
    {
        (f32) -56,
        (f32) 4276,
        (f32) 26150,
        (f32) 2394,
    },
    {
        (f32) -48,
        (f32) 3971,
        (f32) 26217,
        (f32) 2628,
    },
    {
        (f32) -40,
        (f32) 3679,
        (f32) 26262,
        (f32) 2873,
    },
    {
        (f32) -33,
        (f32) 3398,
        (f32) 26285,
        (f32) 3129,
    },
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline int32_t clamp32(int64_t v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (int32_t)v;
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(rspa.buf.as_u8 + addr, 0, nbytes);
}

void aLoadBufferImpl(const void *source_addr) {
    n64_memcpy(rspa.buf.as_u8 + rspa.in, source_addr, ROUND_UP_8(rspa.nbytes));
}

void aSaveBufferImpl(int16_t *dest_addr) {
    n64_memcpy(dest_addr, rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_8(rspa.nbytes));
}

#define recip8192 0.00012207f
#define recip2048 0.00048828f
#define recip2560 0.00039062f

void aLoadADPCMImpl(int num_entries_times_16, const int16_t* book_source_addr) {
    float* aptr = (float*) rspa.adpcm_table;
    short tmp[8];

    __builtin_prefetch(book_source_addr);

    for (int i = 0; i < num_entries_times_16 / 2; i += 8) {
        __builtin_prefetch(&aptr[i]);

        tmp[0] = (short)((uint16_t) book_source_addr[i + 0]);
        tmp[1] = (short)((uint16_t) book_source_addr[i + 1]);
        tmp[2] = (short)((uint16_t) book_source_addr[i + 2]);
        tmp[3] = (short)((uint16_t) book_source_addr[i + 3]);
        tmp[4] = (short)((uint16_t) book_source_addr[i + 4]);
        tmp[5] = (short)((uint16_t) book_source_addr[i + 5]);
        tmp[6] = (short)((uint16_t) book_source_addr[i + 6]);
        tmp[7] = (short)((uint16_t) book_source_addr[i + 7]);

        MEM_BARRIER_PREF(&book_source_addr[i + 8]);

        aptr[i + 0] = recip2048 * (f32) (s32) tmp[0];
        aptr[i + 1] = recip2048 * (f32) (s32) tmp[1];
        aptr[i + 2] = recip2048 * (f32) (s32) tmp[2];
        aptr[i + 3] = recip2048 * (f32) (s32) tmp[3];
        aptr[i + 4] = recip2048 * (f32) (s32) tmp[4];
        aptr[i + 5] = recip2048 * (f32) (s32) tmp[5];
        aptr[i + 6] = recip2048 * (f32) (s32) tmp[6];
        aptr[i + 7] = recip2048 * (f32) (s32) tmp[7];
    }
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    if (flags & A_AUX) {
        rspa.dry_right = in;
    } else {
        rspa.in = in;
        rspa.out = out;
        rspa.nbytes = nbytes;
    }
}

void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) {
    if (flags & A_AUX) {
        rspa.vol_dry = v;
    } else if (flags & A_VOL) {
        if (flags & A_LEFT) {
            rspa.vol[0] = v;
        } else {
            rspa.vol[1] = v;
        }
    } else {
        if (flags & A_LEFT) {
            rspa.target[0] = v;
            rspa.rate[0] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        } else {
            rspa.target[1] = v;
            rspa.rate[1] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        }
    }
}

void aInterleaveImpl(uint16_t left, uint16_t right) {
    int count = ROUND_UP_16(rspa.nbytes) / sizeof(int16_t) / 4;
    int32_t *l = (int32_t *) BUF_S16(left);
    int32_t *r = (int32_t *) BUF_S16(right);

    // int16_t* d = BUF_S16(rspa.out);
    int32_t *d = (int32_t *) (((uintptr_t) BUF_S16(rspa.out) + 3) & ~3);

    __builtin_prefetch(r);

    while (count > 0) {
        __builtin_prefetch(r + 16);
        int32_t l12 = *l++;
        int32_t l34 = *l++;
        int32_t r12 = *r++;
        int32_t r34 = *r++;

        asm volatile("" : : : "memory");

        int32_t lr0 = ((r12 & 0xffff) << 16) | (l12 & 0xffff);
        int32_t lr1 = (((r12 >> 16) & 0xffff) << 16) | ((l12 >> 16) & 0xffff);
        int32_t lr2 = ((r34 & 0xffff) << 16) | (l34 & 0xffff);
        int32_t lr3 = (((r34 >> 16) & 0xffff) << 16) | ((l34 >> 16) & 0xffff);

#if 1
        asm volatile("" : : : "memory");
#endif
        *d++ = lr0;
        *d++ = lr1;
        *d++ = lr2;
        *d++ = lr3;

        --count;
    }
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memmove(rspa.buf.as_u8 + out_addr, rspa.buf.as_u8 + in_addr, nbytes);
}

void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) {
    rspa.adpcm_loop_state = adpcm_loop_state;
}
#include <kos.h>


#include "sh4zam.h"

inline static void shz_xmtrx_load_3x4_rows(const shz_vec4_t* r1, const shz_vec4_t* r2, const shz_vec4_t* r3) {
    asm volatile(R"(
        pref    @%0
        frchg

        fldi0   fr12
        fldi0   fr13
        fldi0   fr14
        fldi1   fr15

        pref    @%1
        fmov.s  @%0+, fr0
        fmov.s  @%0+, fr1
        fmov.s  @%0+, fr2
        fmov.s  @%0,  fr3

        pref    @%2
        fmov.s  @%1+, fr4
        fmov.s  @%1+, fr5
        fmov.s  @%1+, fr6
        fmov.s  @%1,  fr7

        fmov.s  @%2+, fr8
        fmov.s  @%2+, fr9
        fmov.s  @%2+, fr10
        fmov.s  @%2,  fr11

        frchg
    )"
                 : "+&r"(r1), "+&r"(r2), "+&r"(r3));
}

SHZ_FORCE_INLINE void shz_copy_16_shorts(void* restrict dst, const void* restrict src) {
    asm volatile(R"(
        mov.w   @%[s]+, r0
        mov.w   @%[s]+, r1
        mov.w   @%[s]+, r2
        mov.w   @%[s]+, r3
        mov.w   @%[s]+, r4
        mov.w   @%[s]+, r5
        mov.w   @%[s]+, r6
        mov.w   @%[s]+, r7
        add     #16, %[d]
        mov.w   r7, @-%[d]
        mov.w   r6, @-%[d]
        mov.w   r5, @-%[d]
        mov.w   r4, @-%[d]
        mov.w   r3, @-%[d]
        mov.w   r2, @-%[d]
        mov.w   r1, @-%[d]
        mov.w   r0, @-%[d]
        mov.w   @%[s]+, r0
        mov.w   @%[s]+, r1
        mov.w   @%[s]+, r2
        mov.w   @%[s]+, r3
        mov.w   @%[s]+, r4
        mov.w   @%[s]+, r5
        mov.w   @%[s]+, r6
        mov.w   @%[s]+, r7
        add     #32, %[d]
        mov.w   r7, @-%[d]
        mov.w   r6, @-%[d]
        mov.w   r5, @-%[d]
        mov.w   r4, @-%[d]
        mov.w   r3, @-%[d]
        mov.w   r2, @-%[d]
        mov.w   r1, @-%[d]
        mov.w   r0, @-%[d]
    )"
                 : [d] "+r"(dst), [s] "+r"(src)
                 :
                 : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "memory");
}

SHZ_FORCE_INLINE void shz_zero_16_shorts(void* dst) {
    asm volatile(R"(
        xor     r0, r0
        add     #32 %0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
        mov.w   r0, @-%0
    )"
                 :
                 : "r"(dst)
                 : "r0", "memory");
}

static inline s16 clamp16f(float v) {
    // v *= recip2048;
    s32 sv = (s32)v;
    if (sv < -32768) {
        return -32768;
    } else if (sv > 32767) {
        return 32767;
    }
    return (s16)sv;
}

static inline float shift_to_float_multiplier(uint8_t shift) {
    const static float
        __attribute__((aligned(32))) shift_to_float[16] = { 1.0f,    2.0f,    4.0f,     8.0f,    16.0f,   32.0f,
                                                            64.0f,   128.0f,  256.0f,   512.0f,  1024.0f, 2048.0f,
                                                            4096.0f, 8192.0f, 16364.0f, 32768.0f };
    return shift_to_float[shift];
}

static const float __attribute__((aligned(32))) nyblls_as_floats[256][2] = {
    { 0.0f, 0.0f },   { 0.0f, 1.0f },   { 0.0f, 2.0f },   { 0.0f, 3.0f },   { 0.0f, 4.0f },   { 0.0f, 5.0f },
    { 0.0f, 6.0f },   { 0.0f, 7.0f },   { 0.0f, -8.0f },  { 0.0f, -7.0f },  { 0.0f, -6.0f },  { 0.0f, -5.0f },
    { 0.0f, -4.0f },  { 0.0f, -3.0f },  { 0.0f, -2.0f },  { 0.0f, -1.0f },  { 1.0f, 0.0f },   { 1.0f, 1.0f },
    { 1.0f, 2.0f },   { 1.0f, 3.0f },   { 1.0f, 4.0f },   { 1.0f, 5.0f },   { 1.0f, 6.0f },   { 1.0f, 7.0f },
    { 1.0f, -8.0f },  { 1.0f, -7.0f },  { 1.0f, -6.0f },  { 1.0f, -5.0f },  { 1.0f, -4.0f },  { 1.0f, -3.0f },
    { 1.0f, -2.0f },  { 1.0f, -1.0f },  { 2.0f, 0.0f },   { 2.0f, 1.0f },   { 2.0f, 2.0f },   { 2.0f, 3.0f },
    { 2.0f, 4.0f },   { 2.0f, 5.0f },   { 2.0f, 6.0f },   { 2.0f, 7.0f },   { 2.0f, -8.0f },  { 2.0f, -7.0f },
    { 2.0f, -6.0f },  { 2.0f, -5.0f },  { 2.0f, -4.0f },  { 2.0f, -3.0f },  { 2.0f, -2.0f },  { 2.0f, -1.0f },
    { 3.0f, 0.0f },   { 3.0f, 1.0f },   { 3.0f, 2.0f },   { 3.0f, 3.0f },   { 3.0f, 4.0f },   { 3.0f, 5.0f },
    { 3.0f, 6.0f },   { 3.0f, 7.0f },   { 3.0f, -8.0f },  { 3.0f, -7.0f },  { 3.0f, -6.0f },  { 3.0f, -5.0f },
    { 3.0f, -4.0f },  { 3.0f, -3.0f },  { 3.0f, -2.0f },  { 3.0f, -1.0f },  { 4.0f, 0.0f },   { 4.0f, 1.0f },
    { 4.0f, 2.0f },   { 4.0f, 3.0f },   { 4.0f, 4.0f },   { 4.0f, 5.0f },   { 4.0f, 6.0f },   { 4.0f, 7.0f },
    { 4.0f, -8.0f },  { 4.0f, -7.0f },  { 4.0f, -6.0f },  { 4.0f, -5.0f },  { 4.0f, -4.0f },  { 4.0f, -3.0f },
    { 4.0f, -2.0f },  { 4.0f, -1.0f },  { 5.0f, 0.0f },   { 5.0f, 1.0f },   { 5.0f, 2.0f },   { 5.0f, 3.0f },
    { 5.0f, 4.0f },   { 5.0f, 5.0f },   { 5.0f, 6.0f },   { 5.0f, 7.0f },   { 5.0f, -8.0f },  { 5.0f, -7.0f },
    { 5.0f, -6.0f },  { 5.0f, -5.0f },  { 5.0f, -4.0f },  { 5.0f, -3.0f },  { 5.0f, -2.0f },  { 5.0f, -1.0f },
    { 6.0f, 0.0f },   { 6.0f, 1.0f },   { 6.0f, 2.0f },   { 6.0f, 3.0f },   { 6.0f, 4.0f },   { 6.0f, 5.0f },
    { 6.0f, 6.0f },   { 6.0f, 7.0f },   { 6.0f, -8.0f },  { 6.0f, -7.0f },  { 6.0f, -6.0f },  { 6.0f, -5.0f },
    { 6.0f, -4.0f },  { 6.0f, -3.0f },  { 6.0f, -2.0f },  { 6.0f, -1.0f },  { 7.0f, 0.0f },   { 7.0f, 1.0f },
    { 7.0f, 2.0f },   { 7.0f, 3.0f },   { 7.0f, 4.0f },   { 7.0f, 5.0f },   { 7.0f, 6.0f },   { 7.0f, 7.0f },
    { 7.0f, -8.0f },  { 7.0f, -7.0f },  { 7.0f, -6.0f },  { 7.0f, -5.0f },  { 7.0f, -4.0f },  { 7.0f, -3.0f },
    { 7.0f, -2.0f },  { 7.0f, -1.0f },  { -8.0f, 0.0f },  { -8.0f, 1.0f },  { -8.0f, 2.0f },  { -8.0f, 3.0f },
    { -8.0f, 4.0f },  { -8.0f, 5.0f },  { -8.0f, 6.0f },  { -8.0f, 7.0f },  { -8.0f, -8.0f }, { -8.0f, -7.0f },
    { -8.0f, -6.0f }, { -8.0f, -5.0f }, { -8.0f, -4.0f }, { -8.0f, -3.0f }, { -8.0f, -2.0f }, { -8.0f, -1.0f },
    { -7.0f, 0.0f },  { -7.0f, 1.0f },  { -7.0f, 2.0f },  { -7.0f, 3.0f },  { -7.0f, 4.0f },  { -7.0f, 5.0f },
    { -7.0f, 6.0f },  { -7.0f, 7.0f },  { -7.0f, -8.0f }, { -7.0f, -7.0f }, { -7.0f, -6.0f }, { -7.0f, -5.0f },
    { -7.0f, -4.0f }, { -7.0f, -3.0f }, { -7.0f, -2.0f }, { -7.0f, -1.0f }, { -6.0f, 0.0f },  { -6.0f, 1.0f },
    { -6.0f, 2.0f },  { -6.0f, 3.0f },  { -6.0f, 4.0f },  { -6.0f, 5.0f },  { -6.0f, 6.0f },  { -6.0f, 7.0f },
    { -6.0f, -8.0f }, { -6.0f, -7.0f }, { -6.0f, -6.0f }, { -6.0f, -5.0f }, { -6.0f, -4.0f }, { -6.0f, -3.0f },
    { -6.0f, -2.0f }, { -6.0f, -1.0f }, { -5.0f, 0.0f },  { -5.0f, 1.0f },  { -5.0f, 2.0f },  { -5.0f, 3.0f },
    { -5.0f, 4.0f },  { -5.0f, 5.0f },  { -5.0f, 6.0f },  { -5.0f, 7.0f },  { -5.0f, -8.0f }, { -5.0f, -7.0f },
    { -5.0f, -6.0f }, { -5.0f, -5.0f }, { -5.0f, -4.0f }, { -5.0f, -3.0f }, { -5.0f, -2.0f }, { -5.0f, -1.0f },
    { -4.0f, 0.0f },  { -4.0f, 1.0f },  { -4.0f, 2.0f },  { -4.0f, 3.0f },  { -4.0f, 4.0f },  { -4.0f, 5.0f },
    { -4.0f, 6.0f },  { -4.0f, 7.0f },  { -4.0f, -8.0f }, { -4.0f, -7.0f }, { -4.0f, -6.0f }, { -4.0f, -5.0f },
    { -4.0f, -4.0f }, { -4.0f, -3.0f }, { -4.0f, -2.0f }, { -4.0f, -1.0f }, { -3.0f, 0.0f },  { -3.0f, 1.0f },
    { -3.0f, 2.0f },  { -3.0f, 3.0f },  { -3.0f, 4.0f },  { -3.0f, 5.0f },  { -3.0f, 6.0f },  { -3.0f, 7.0f },
    { -3.0f, -8.0f }, { -3.0f, -7.0f }, { -3.0f, -6.0f }, { -3.0f, -5.0f }, { -3.0f, -4.0f }, { -3.0f, -3.0f },
    { -3.0f, -2.0f }, { -3.0f, -1.0f }, { -2.0f, 0.0f },  { -2.0f, 1.0f },  { -2.0f, 2.0f },  { -2.0f, 3.0f },
    { -2.0f, 4.0f },  { -2.0f, 5.0f },  { -2.0f, 6.0f },  { -2.0f, 7.0f },  { -2.0f, -8.0f }, { -2.0f, -7.0f },
    { -2.0f, -6.0f }, { -2.0f, -5.0f }, { -2.0f, -4.0f }, { -2.0f, -3.0f }, { -2.0f, -2.0f }, { -2.0f, -1.0f },
    { -1.0f, 0.0f },  { -1.0f, 1.0f },  { -1.0f, 2.0f },  { -1.0f, 3.0f },  { -1.0f, 4.0f },  { -1.0f, 5.0f },
    { -1.0f, 6.0f },  { -1.0f, 7.0f },  { -1.0f, -8.0f }, { -1.0f, -7.0f }, { -1.0f, -6.0f }, { -1.0f, -5.0f },
    { -1.0f, -4.0f }, { -1.0f, -3.0f }, { -1.0f, -2.0f }, { -1.0f, -1.0f }
};

static inline void extend_nyblls_to_floats(uint8_t nybll, float* fp1, float* fp2) {
    const float* fpair = nyblls_as_floats[nybll];
    *fp1 = fpair[0];
    *fp2 = fpair[1];
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
    int16_t* out = BUF_S16(rspa.out);
    MEM_BARRIER_PREF(out);
    uint8_t* in = BUF_U8(rspa.in);
    int nbytes = ROUND_UP_32(rspa.nbytes);
    if (flags & A_INIT) {
        shz_zero_16_shorts(out);
    } else if (flags & A_LOOP) {
        shz_copy_16_shorts(out, rspa.adpcm_loop_state);
        for (int i=0;i<16;i++) {
            out[i] = __builtin_bswap16(out[i]);
        }
    } else {
        shz_copy_16_shorts(out, state);
    }
    MEM_BARRIER_PREF(in);
    out += 16;
    float prev1 = out[-1];
    float prev2 = out[-2];

    while (nbytes > 0) {
        const uint8_t si_in = *in++;
        const uint8_t next = *in++;
        MEM_BARRIER_PREF(nyblls_as_floats[next]);
        const uint8_t in_array[2][4] = {
            { next, *in++, *in++, *in++ },
            { *in++, *in++, *in++, *in++ }
        };
        const unsigned table_index = si_in & 0xf; // should be in 0..7
        const float(*tbl)[8] = rspa.adpcm_table[table_index];
        const float shift = shift_to_float_multiplier(si_in >> 4); // should be in 0..12 or 0..14
        float instr[2][8];

        for(int i = 0; i < 2; ++i) {
            {
                MEM_BARRIER_PREF(nyblls_as_floats[in_array[i][1]]);
                extend_nyblls_to_floats(in_array[i][0], &instr[i][0], &instr[i][1]);
                instr[i][0] *= shift;
                instr[i][1] *= shift;
                MEM_BARRIER_PREF(nyblls_as_floats[in_array[i][2]]);
                extend_nyblls_to_floats(in_array[i][1], &instr[i][2], &instr[i][3]);
                instr[i][2] *= shift;
                instr[i][3] *= shift;
            }
            {
                MEM_BARRIER_PREF(nyblls_as_floats[in_array[i][3]]);
                extend_nyblls_to_floats(in_array[i][2], &instr[i][4], &instr[i][5]);
                instr[i][4] *= shift;
                instr[i][5] *= shift;
                MEM_BARRIER_PREF(&tbl[i][0]);
                extend_nyblls_to_floats(in_array[i][3], &instr[i][6], &instr[i][7]);
                instr[i][6] *= shift;
                instr[i][7] *= shift;
            }
        }
        MEM_BARRIER_PREF(in);

        for (size_t i = 0; i < 2; i++) {
            const float *ins = instr[i];
            shz_vec4_t acc_vec[2];
            float *accf = (float *)acc_vec;
            const shz_vec4_t in_vec = { .x = prev2, .y = prev1, .z = 1.0f };

            shz_xmtrx_load_3x4_rows((const shz_vec4_t*)&tbl[0][0], (const shz_vec4_t*)&tbl[1][0], (const shz_vec4_t*)&ins[0]);
            acc_vec[0] = shz_xmtrx_trans_vec4(in_vec);
            shz_xmtrx_load_3x4_rows((const shz_vec4_t*)&tbl[0][4], (const shz_vec4_t*)&tbl[1][4], (const shz_vec4_t*)&ins[4]);
            acc_vec[1] = shz_xmtrx_trans_vec4(in_vec);

            {
                register float fone asm("fr8")  = 1.0f;
                register float ins0 asm("fr9")  = ins[0];
                register float ins1 asm("fr10") = ins[1];
                register float ins2 asm("fr11") = ins[2];
                accf[2] = shz_dot8f(fone, ins0, ins1, ins2, accf[2], tbl[1][1], tbl[1][0], 0.0f);
                accf[7] = shz_dot8f(fone, ins0, ins1, ins2, accf[7], tbl[1][6], tbl[1][5], tbl[1][4]);
                accf[1] += (tbl[1][0] * ins0);
                shz_xmtrx_load_4x4_cols((const shz_vec4_t*)&accf[3], (const shz_vec4_t*)&tbl[1][2], (const shz_vec4_t*)&tbl[1][1], (const shz_vec4_t*)&tbl[1][0]);
                *(SHZ_ALIASING shz_vec4_t*)&accf[3] =
                    shz_xmtrx_trans_vec4((shz_vec4_t) { .x = fone, .y = ins0, .z = ins1, .w = ins2 });
            }
            {
                register float ins3 asm("fr8")  = ins[3];
                register float ins4 asm("fr9")  = ins[4];
                register float ins5 asm("fr10") = ins[5];
                register float ins6 asm("fr11") = ins[6];
                accf[7] += shz_dot8f(ins3, ins4, ins5, ins6, tbl[1][3], tbl[1][2], tbl[1][1], tbl[1][0]);
                accf[6] += shz_dot8f(ins3, ins4, ins5, ins6, tbl[1][2], tbl[1][1], tbl[1][0], 0.0f);
                accf[5] += (tbl[1][1] * ins3) + (tbl[1][0] * ins4);
                accf[4] += (tbl[1][0] * ins3);
            }

            for (size_t j = 0; j < 6; ++j)
                *out++ = clamp16f(accf[j]);

            prev2  = clamp16f(accf[6]);
            *out++ = prev2;
            prev1  = clamp16f(accf[7]);
            *out++ = prev1;
        }
        MEM_BARRIER_PREF(out);
        nbytes -= 16 * sizeof(int16_t);
    }

    shz_copy_16_shorts(state, (out - 16));
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t __attribute__((aligned(32))) tmp[32] = { 0 };
    int16_t* in_initial = BUF_S16(rspa.in);
    int16_t* in = in_initial;
    MEM_BARRIER_PREF(in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator = 0;
    int i = 0;
    float* tbl_f = NULL;
    float sample_f = 0;
    size_t l;

    int16_t *dp, *sp;
    int32_t *wdp, *wsp;

    if (!(flags & A_INIT)) {
        dp = tmp;
        sp = state;

        wdp = (int32_t *)dp;
        wsp = (int32_t *)sp;

        if ((((uintptr_t)wdp | (uintptr_t)wsp) & 3) == 0) {
            for (l = 0; l < 8; l++)
                *wdp++ = *wsp++;
        } else {
            for (l = 0; l < 16; l++)
                *dp++ = *sp++;
        }
    }

    in -= 4;
    pitch_accumulator = (uint16_t) tmp[4];
    tbl_f = resample_table[pitch_accumulator >> 10];
    __builtin_prefetch(tbl_f);

    dp = in;
    sp = tmp;
    for (l = 0; l < 4; l++)
        *dp++ = *sp++;

    do {
        __builtin_prefetch(out);
        for (i = 0; i < 8; i++) {

            float in_f[4] = { (float) (int) in[0], (float) (int) in[1], (float) (int) in[2], (float) (int) in[3] };

            sample_f =
                shz_dot8f(in_f[0], in_f[1], in_f[2], in_f[3], tbl_f[0], tbl_f[1], tbl_f[2], tbl_f[3]) * 0.00003052f;

            MEM_BARRIER();
            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            MEM_BARRIER_PREF(in);
            pitch_accumulator %= 0x10000;
            MEM_BARRIER();
            *out++ = clamp16f((sample_f));
            MEM_BARRIER();
            tbl_f = resample_table[pitch_accumulator >> 10];
            MEM_BARRIER_PREF(tbl_f);
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    state[4] = (int16_t) pitch_accumulator;
    dp = (int16_t*) (state);
    sp = in;
    for (l = 0; l < 4; l++)
        *dp++ = *sp++;

    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    dp = (int16_t*) (state + 8);
    sp = in;
    for (l = 0; l < 8; l++)
        *dp++ = *sp++;
}

/*@Note: Much Slowdown */
void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state) {
    int32_t *in = (int32_t*)(rspa.buf.as_s16 + rspa.in / sizeof(int16_t));
    int32_t *dry[2] = { (int32_t*)(rspa.buf.as_s16 + rspa.out / sizeof(int16_t)),  (int32_t*)(rspa.buf.as_s16 + rspa.dry_right / sizeof(int16_t))};
    int nbytes = ROUND_UP_16(rspa.nbytes);

    int16_t target[2];
    int32_t rate[2];
    int16_t vol_dry;//, vol_wet;

    int32_t step_diff[2];
    int32_t vols[2][8];

    int c, i;

    if (flags & A_INIT) {
        target[0] = rspa.target[0];
        target[1] = rspa.target[1];
        rate[0] = rspa.rate[0];
        rate[1] = rspa.rate[1];
        vol_dry = rspa.vol_dry;
        step_diff[0] = (rspa.vol[0] * (rate[0] - 0x10000))>>3;// / 8;
        step_diff[1] = (rspa.vol[0] * (rate[1] - 0x10000))>>3;// / 8;

        for (i = 0; i < 8; i++) {
            vols[0][i] = clamp32((int64_t)(rspa.vol[0] << 16) + step_diff[0] * (i + 1));
            vols[1][i] = clamp32((int64_t)(rspa.vol[1] << 16) + step_diff[1] * (i + 1));
        }
    } else {
        n64_memcpy(vols[0], state, 32);
        n64_memcpy(vols[1], state + 16, 32);
        target[0] = state[32];
        target[1] = state[35];
        rate[0] = (state[33] << 16) | (uint16_t)state[34];
        rate[1] = (state[36] << 16) | (uint16_t)state[37];
        vol_dry = state[38];
    }

    do {
        int32_t ratec;
        for (c = 0; c < 2; c++) {
            int32_t *volsc = vols[c];
            int32_t *dryc = dry[c];
            int32_t targetc = target[c] << 16;
            ratec = rate[c];
            for (i = 0; i < 8; i+=2) {
                if ((ratec >> 16) > 0) {
                    // Increasing volume
                    if (volsc[i] > targetc) {
                        volsc[i] = targetc;
                    }
                    if (volsc[i+1] > targetc) {
                        volsc[i+1] = targetc;
                    }
                } else {
                    // Decreasing volume
                    if (volsc[i] < targetc) {
                        volsc[i] = targetc;
                    }
                    if (volsc[i+1] < targetc) {
                        volsc[i+1] = targetc;
                    }
                }
                int32_t dryc12 = dryc[i>>1];
                int16_t dryc1 = (dryc12 >>16) & 0xffff;
                int16_t dryc2 = (dryc12      ) & 0xffff;
                int32_t in12 = in[i>>1];
                int16_t in1 = (in12 >>16) & 0xffff;
                int16_t in2 = (in12      ) & 0xffff;


                int16_t mixed1 = clamp16((dryc1 * 0x7fff + in1 * (((volsc[i] >> 16) * vol_dry + 0x4000) >> 15) + 0x4000) >> 15);
                int16_t mixed2 = clamp16((dryc2 * 0x7fff + in2 * (((volsc[i+1] >> 16) * vol_dry + 0x4000) >> 15) + 0x4000) >> 15);

//                dryc[i] = clamp16((dryc[i] * 0x7fff + in[i] * (((volsc[i] >> 16) * vol_dry + 0x4000) >> 15) + 0x4000) >> 15);
                dryc[i>>1] = (mixed1 << 16) | (mixed2 & 0xffff);
                volsc[i] = clamp32((int64_t)volsc[i] * ratec >> 16);
                volsc[i+1] = clamp32((int64_t)volsc[i+1] * ratec >> 16);
            }

            dry[c] += 4;
        }

        nbytes -= 16;
        in += 4;//8;
    } while (nbytes > 0);

    n64_memcpy(state, vols[0], 32);
    n64_memcpy(state + 16, vols[1], 32);
    state[32] = target[0];
    state[35] = target[1];
    state[33] = (int16_t)(rate[0] >> 16);
    state[34] = (int16_t)rate[0];
    state[36] = (int16_t)(rate[1] >> 16);
    state[37] = (int16_t)rate[1];
    state[38] = vol_dry;
}

#if 0
/*@Note: Yes Slowdown */
void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_32(rspa.nbytes);
    int16_t *in = rspa.buf.as_s16 + in_addr / sizeof(int16_t);
    int16_t *out = rspa.buf.as_s16 + out_addr / sizeof(int16_t);
    int i;
    int32_t sample;

    if (gain == -0x8000) {
        while (nbytes > 0) {

            for (i = 0; i < 16; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }

            nbytes -= 16 * sizeof(int16_t);
        }
    }

    while (nbytes > 0) {
        for (i = 0; i < 16; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }
        nbytes -= 16 * sizeof(int16_t);
    }
}
#endif

#define LO16(x) ((int16_t)((x) & 0xffff))
#define HI16(x) ((int16_t)((x) >> 16))
#define PACK16(lo, hi) ((uint32_t)((uint16_t)(lo) | ((uint32_t)(uint16_t)(hi) << 16)))
static inline int16_t clamp16_i32(int32_t x) {
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}
void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr)
{
    int nbytes = ROUND_UP_32(rspa.nbytes);

    uint32_t *in  = (uint32_t *)(rspa.buf.as_s16 + in_addr  / 2);
    uint32_t *out = (uint32_t *)(rspa.buf.as_s16 + out_addr / 2);

    if (gain == -0x8000) {
        while (nbytes > 0) {

            // 16 samples = 8 packed words
            for (int i = 0; i < 8; i++) {
                uint32_t o = *out;
                uint32_t s = *in++;

                int32_t a0 = LO16(o) - LO16(s);
                int32_t a1 = HI16(o) - HI16(s);

                *out++ = PACK16(
                    clamp16_i32(a0),
                    clamp16_i32(a1)
                );
            }

            nbytes -= 32;
        }
        return;
    }

    const int32_t gain32 = gain;

    while (nbytes > 0) {
        for (int i = 0; i < 8; i++) {
            uint32_t o = *out;
            uint32_t s = *in++;

            int32_t o0 = LO16(o);
            int32_t o1 = HI16(o);
            int32_t s0 = LO16(s);
            int32_t s1 = HI16(s);

            int32_t r0 = (o0 * 0x7fff + s0 * gain32 + 0x4000) >> 15;
            int32_t r1 = (o1 * 0x7fff + s1 * gain32 + 0x4000) >> 15;

            *out++ = PACK16(
                clamp16_i32(r0),
                clamp16_i32(r1)
            );
        }

        nbytes -= 32;
    }
}
