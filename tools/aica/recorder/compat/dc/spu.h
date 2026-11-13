// What src/audio/aica_synth.c includes from KOS, for tools/aica/recorder/main.c.
#ifndef COMPAT_DC_SPU_H
#define COMPAT_DC_SPU_H
#include <stddef.h>
#include <stdint.h>
void spu_memload_sq(uintptr_t to, void *from, size_t length);
void spu_memread(void *to, uintptr_t from, size_t length);
uint32_t snd_mem_malloc(size_t size);
void snd_mem_free(uint32_t addr);
#include <arch/timer.h>
#endif
