// The recorder's clock (tools/aica/recorder/main.c) stands in for KOS's.
#ifndef COMPAT_ARCH_TIMER_H
#define COMPAT_ARCH_TIMER_H
#include <stdint.h>
uint64_t timer_us_gettime64(void);
#endif
