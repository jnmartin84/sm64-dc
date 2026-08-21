/*
 * File: dc_audio_kos.c
 * Project: sm64-port
 * Author: Hayden Kowalchuk (hayden@hkowsoftware.com)
 * -----
 * Copyright (c) 2025 Hayden Kowalchuk
 */

/*
 * Modifications by jnmartin84
 *
 * AICA hardware mixing: the KOS sound stream is no longer used. The AICA voice
 * driver (src/audio/aica_synth.c) plays voices directly on the AICA hardware
 * channels via the SH4->AICA command queue. This backend only brings up the
 * base sound system (snd_init) that the voice driver relies on; it produces no
 * output of its own. n64_memcpy lives here (used project-wide) and is retained.
 */

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <stdbool.h>

#include "audio_dc.h"
#include "macros.h"

#define MEM_BARRIER() asm volatile("" : : : "memory");
#define MEM_BARRIER_PREF(ptr) asm volatile("pref @%0" : : "r"((ptr)) : "memory")

void n64_memcpy(void* dst, const void* src, size_t size) {
    __builtin_prefetch(src);
    uint8_t* bdst = (uint8_t*) dst;
    uint8_t* bsrc = (uint8_t*) src;
    uint16_t* sdst = (uint16_t*) dst;
    uint16_t* ssrc = (uint16_t*) src;
    uint32_t* wdst = (uint32_t*) dst;
    uint32_t* wsrc = (uint32_t*) src;

    int size_to_copy = size;
    int words_to_copy = size_to_copy >> 2;
    int shorts_to_copy = size_to_copy >> 1;
    int bytes_to_copy = size_to_copy - (words_to_copy << 2);
    int sbytes_to_copy = size_to_copy - (shorts_to_copy << 1);

    if ((!(((uintptr_t) bdst | (uintptr_t) bsrc) & 3))) {
        while (words_to_copy--) {
            *wdst++ = *wsrc++;
        }

        bdst = (uint8_t*) wdst;
        bsrc = (uint8_t*) wsrc;

        switch (bytes_to_copy) {
            case 0:
                return;
            case 1:
                goto n64copy1;
            case 2:
                goto n64copy2;
            case 3:
                goto n64copy3;
            default:
                return;
        }
    } else if ((!(((uintptr_t) sdst | (uintptr_t) ssrc) & 1))) {
        while (shorts_to_copy--) {
            *sdst++ = *ssrc++;
        }

        bdst = (uint8_t*) sdst;
        bsrc = (uint8_t*) ssrc;

        if (sbytes_to_copy) {
            goto n64copy1;
        }

        return;
    } else {
        while (words_to_copy-- > 0) {
            uint8_t b1, b2, b3, b4;
            b1 = *bsrc++;
            b2 = *bsrc++;
            b3 = *bsrc++;
            b4 = *bsrc++;

            MEM_BARRIER();

            *bdst++ = b1;
            *bdst++ = b2;
            *bdst++ = b3;
            *bdst++ = b4;
        }

        switch (bytes_to_copy) {
            case 0:
                return;
            case 1:
                goto n64copy1;
            case 2:
                goto n64copy2;
            case 3:
                goto n64copy3;
            default:
                return;
        }
    }

n64copy3:
    *bdst++ = *bsrc++;
n64copy2:
    *bdst++ = *bsrc++;
n64copy1:
    *bdst = *bsrc;
    return;
}

/* --- Base sound system bring-up (no stream) ---
   snd_init uploads the AICA ARM program and sets up snd_mem/the ARAM allocator
   and the SH4->AICA command queue, which aica_synth.c uses directly. We do NOT
   allocate or start a KOS stream, so all 64 AICA channels are free. */
static bool audio_dc_init(void) {
    /* snd_stream_init() == snd_init() + stream-subsystem bring-up. We allocate
       and start NO stream (0 AICA channels consumed), but this matches the
       proven MK64/OoT init path that fully sets up the AICA output. */
    if (snd_stream_init() != 0) {
        printf("AICA INIT FAILURE!\n");
        return false;
    }
    /* keep the scheduler lively for the vblank-driven synthesis thread */
    thd_set_hz(300);
    return true;
}

static int audio_dc_buffered(void) {
    return 0;
}

static int audio_dc_get_desired_buffered(void) {
    return 0;
}

/* AICA mixes in hardware; there is no software-mixed output to push. */
static void audio_dc_play(UNUSED uint8_t *bufL, UNUSED uint8_t *bufR, UNUSED size_t len) {
}

struct AudioAPI audio_dc = {
    audio_dc_init,
    audio_dc_buffered,
    audio_dc_get_desired_buffered,
    audio_dc_play
};
