#ifndef _ULTRA64_TYPES_H_
#define _ULTRA64_TYPES_H_

#ifndef NULL
#define NULL    (void *)0
#endif

#define TRUE 1
#define FALSE 0

typedef signed char            s8;
typedef unsigned char          u8;
typedef signed short int       s16;
typedef unsigned short int     u16;
typedef signed int             s32;
typedef unsigned int           u32;

typedef float  f32;
#ifdef TARGET_DC
// The SH4 build uses -m4-single, where double is a real 64-bit type: every f64 operation switches
// FPSCR to double precision, and GCC can then schedule sh4zam's single-precision FPU asm (fsrra,
// fipr, ftrv) inside the double-precision window, where it computes garbage (goddard's gd_sqrt_d
// hung the title screen that way). SM64's f64 math never needed doubles; keep it single, as the
// original -m4-single-only port did.
typedef float f64;
#else
typedef double f64;
#endif

#ifdef TARGET_N64
typedef signed long long int   s64;
typedef unsigned long long int u64;
typedef u32 size_t;
typedef s32 ssize_t;
typedef u32 uintptr_t;
typedef s32 intptr_t;
typedef s32 ptrdiff_t;
#else
#include <stddef.h>
#include <stdint.h>
typedef ptrdiff_t ssize_t;
typedef int64_t s64;
typedef uint64_t u64;
#endif

typedef volatile u8   vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8   vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

#endif
