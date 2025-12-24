#if 0
/*
 * File: dc_audio_kos.c
 * Project: sm64-port
 * Author: Hayden Kowalchuk (hayden@hkowsoftware.com)
 * -----
 * Copyright (c) 2025 Hayden Kowalchuk
 */

/*
 * Modifications by jnmartin84
 */

#include <kos.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "audio_dc.h"
#include "macros.h"



void n64_memcpy(void* dst, const void* src, size_t size) {
    if (!size)
        return;
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

    __builtin_prefetch(bsrc);
    if ((!(((uintptr_t) bdst | (uintptr_t) bsrc) & 3))) {
        while (words_to_copy--) {
            if ((words_to_copy & 3) == 0) {
                __builtin_prefetch(wsrc + 8);
            }
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

// --- Configuration ---
// Stereo
#define DC_AUDIO_CHANNELS (2) 
#define DC_STEREO_AUDIO ( DC_AUDIO_CHANNELS == 2)
// Sample rate for the AICA (32kHz)
#define DC_AUDIO_FREQUENCY (26800)
//(32000) 
#define RING_BUFFER_MAX_BYTES (16384 /* / 2 */)

// --- Global State for Dreamcast Audio Backend ---
// Handle for the sound stream
static volatile snd_stream_hnd_t shnd = SND_STREAM_INVALID; 
// The main audio buffer
static uint8_t cb_buf_internal[RING_BUFFER_MAX_BYTES] __attribute__((aligned(32))); 
static void *const cb_buf = cb_buf_internal;
static bool audio_started = false;

typedef enum {
    AUDIO_STATUS_RUNNING,
    AUDIO_STATUS_DONE
} audio_thread_status_t;

volatile audio_thread_status_t g_audio_thread_status = AUDIO_STATUS_DONE;
static kthread_t *g_audio_poll_thread_handle = NULL;
//static kthread_t *g_audio_gen_thread_handle = NULL;


#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

typedef struct {
    uint8_t *buf;
    uint32_t cap; // power-of-two
    uint32_t head; // next write pos
    uint32_t tail; // next read pos
} ring_t;

static ring_t cb_ring;
static ring_t *r = &cb_ring;

static bool cb_init(size_t capacity) {
    // round capacity up to power of two
    r->cap = 1u << (32 - __builtin_clz(capacity - 1));
    r->buf = cb_buf;
    if (!r->buf)
        return false;
    r->head = 0;
    r->tail = 0;
    return true;
}

void cb_write_data(const void *src, size_t n) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    uint32_t free = r->cap - (head - tail);
    if (n > free)
        return;// 0;
    uint32_t idx = head & (r->cap - 1);
    uint32_t first = MIN(n, r->cap - idx);
    n64_memcpy(r->buf + idx, src, first);
    if (first != n)
        n64_memcpy(r->buf, (uint8_t*)src + first, n - first);
    r->head = head + n;
//    return n;
}

static size_t cb_read_data(void *dst, size_t n) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;atomic_load(&r->tail);
    uint32_t avail = head - tail;
    if (n > avail) return 0;
    uint32_t idx = tail & (r->cap - 1);
    uint32_t first = MIN(n, r->cap - idx);
    n64_memcpy(dst, r->buf + idx, first);
    //n64_memcpy((uint8_t*)dst + first, r->buf, n - first);
    r->tail = tail + n;
    return n;
}

// Calculates and returns the number of bytes currently in the ring buffer
static size_t cb_get_used(void) {
    // Atomically load both head and tail to get a consistent snapshot.
    // The order of loads might matter in some weak memory models,
    // but for head-tail diff, generally not.
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    
    // The number of used bytes is simply the difference between head and tail.
    // This works because head and tail are continuously incrementing indices,
    // and effectively handle wrap-around due to the (head - tail) arithmetic.
    return head - tail;
}

// You might also want a function to get free space:
static size_t cb_get_free(void) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    return r->cap - (head - tail);
}

// And optionally, if you need to check if it's full or empty
static bool cb_is_empty(void) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    return head == tail;
}

static bool cb_is_full(void) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    // A power-of-two ring buffer designed this way typically means
    // full when (head - tail) == cap
    return (head - tail) == r->cap;
}

static void cb_clear(void) {
    ;
}

// --- KOS Stream Audio Callback (Consumer): Called by KOS when the AICA needs more data ---
#define NUM_BUFFER_BLOCKS (2)
#define TEMP_BUF_SIZE ((SND_STREAM_BUFFER_MAX/*  / 2 */) * NUM_BUFFER_BLOCKS)
static uint8_t __attribute__((aligned(32))) temp_buf[TEMP_BUF_SIZE];
static unsigned int temp_buf_sel = 0;
void mute_stream(void) {
    snd_stream_volume(shnd, 0); // Set maximum volume
}

void unmute_stream(void) {
    snd_stream_volume(shnd, 192); // Set maximum volume
}

void *audio_callback(UNUSED snd_stream_hnd_t hnd, int samples_requested_bytes, int *samples_returned_bytes) {
    size_t samples_requested = samples_requested_bytes / 4;
    size_t samples_avail_bytes = cb_read_data(temp_buf + ((4096/*  / 2 */) * temp_buf_sel) , samples_requested_bytes);
    
    *samples_returned_bytes = samples_requested_bytes;
    size_t samples_returned = samples_avail_bytes / 4;
    
    /*@Note: This is more correct, fill with empty audio */
    if (samples_avail_bytes < (unsigned)samples_requested_bytes) {
        memset(temp_buf + ((4096/*  / 2 */) * temp_buf_sel) + samples_avail_bytes, 0, (samples_requested_bytes - samples_avail_bytes));
        // printf("U\n");
    }
    
    temp_buf_sel += 1;
    if (temp_buf_sel >= NUM_BUFFER_BLOCKS) {
        temp_buf_sel = 0;
    }
    
    return (void*)(temp_buf + ((4096/*  / 2 */) * temp_buf_sel));
}
mutex_t reset_mutex;

static bool audio_dc_init(void) {
    if (snd_stream_init()) {
        printf("AICA INIT FAILURE!\n");
        return false;
    }

    thd_set_hz(300);
    
    mutex_init(&reset_mutex, MUTEX_TYPE_NORMAL);

    // --- Initial Pre-fill of Ring Buffer with Silence ---
    sq_clr(cb_buf_internal, sizeof(cb_buf_internal));
    sq_clr(temp_buf, sizeof(temp_buf));
    if (!cb_init(RING_BUFFER_MAX_BYTES)) {
        printf("CB INIT FAILURE!\n");
        return false;
    }
    
    printf("Dreamcast Audio: Initialized. Ring buffer size: %u bytes.\n",
           (unsigned int)RING_BUFFER_MAX_BYTES);
    
    // Allocate the sound stream with KOS
    shnd = snd_stream_alloc(audio_callback, 4096);//(SND_STREAM_BUFFER_MAX / 8) /* / 2 */);
    if (shnd == SND_STREAM_INVALID) {
        printf("SND: Stream allocation failure!\n");
        snd_stream_destroy(shnd);
        return false;
    }

    // Set maximum volume
    snd_stream_volume(shnd, 192); 

    printf("Sound init complete!\n");
    
    return true;
}

static int audio_dc_buffered(void) {
    return 1088;
}

static int audio_dc_get_desired_buffered(void) {
    return 1100;
}


void runtime_reset(void) {
    mutex_lock(&reset_mutex);
    snd_stream_volume(shnd,0);
    audio_started = false;
    // --- Initial Pre-fill of Ring Buffer with Silence ---
    sq_clr(cb_buf_internal, sizeof(cb_buf_internal));
    sq_clr(temp_buf, sizeof(temp_buf));
    if (!cb_init(RING_BUFFER_MAX_BYTES)) {
        printf("CB INIT FAILURE!\n");
    }
    snd_stream_volume(shnd,192);
    mutex_unlock(&reset_mutex);
}

static void audio_dc_play(const uint8_t *buf, size_t len) {
    cb_write_data(buf, len);

    if (!audio_started) {
        audio_started = true;
        snd_stream_start(shnd, DC_AUDIO_FREQUENCY, DC_STEREO_AUDIO);
    }

    if (audio_started) {
        snd_stream_poll(shnd);
    }
}

struct AudioAPI audio_dc = {
    audio_dc_init,
    audio_dc_buffered,
    audio_dc_get_desired_buffered,
    audio_dc_play
};
#endif

/*
 * File: dc_audio_kos.c
 * Project: sm64-port
 * Author: Hayden Kowalchuk (hayden@hkowsoftware.com)
 * -----
 * Copyright (c) 2025 Hayden Kowalchuk
 */

/*
 * Modifications by jnmartin84
 */

#include <kos.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "audio_dc.h"
#include "macros.h"

#define MEM_BARRIER() asm volatile("" : : : "memory");
#define MEM_BARRIER_PREF(ptr) asm volatile("pref @%0" : : "r"((ptr)) : "memory")

void n64_memcpy(void* dst, const void* src, size_t size) {
    if (!size)
        return;
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

//    __builtin_prefetch(bsrc);
    if ((!(((uintptr_t) bdst | (uintptr_t) bsrc) & 3))) {
        while (words_to_copy--) {
    //        if ((words_to_copy & 3) == 0) {
  //              __builtin_prefetch(wsrc + 8);
      //      }
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

// --- Configuration ---
// Stereo
#define DC_AUDIO_CHANNELS (2) 
#define DC_STEREO_AUDIO ( DC_AUDIO_CHANNELS == 2)

#define DC_AUDIO_FREQUENCY (26800)

#define RING_BUFFER_MAX_BYTES (16384)

// --- Global State for Dreamcast Audio Backend ---
// Handle for the sound stream
static volatile snd_stream_hnd_t shnd = SND_STREAM_INVALID; 
// The main audio buffer
static uint8_t __attribute__((aligned(4096))) cb_buf_internal[2][RING_BUFFER_MAX_BYTES]; 
static bool audio_started = false;

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

typedef struct {
    uint8_t *buf;
    uint32_t cap; // power-of-two
    uint32_t head; // next write pos
    uint32_t tail; // next read pos
} ring_t;

static ring_t __attribute__((aligned(32))) cb_ring[2];
static ring_t *r[2] = {&cb_ring[0],&cb_ring[1]};

#if 0
// !USE_TLB_CB
static void *const cb_buf[2] = {cb_buf_internal[0],cb_buf_internal[1]};

static bool cb_init(int N, size_t capacity) {
    // round capacity up to power of two
    r[N]->cap = 1u << (32 - __builtin_clz(capacity - 1));
    r[N]->buf = cb_buf[N];
    if (!r[N]->buf)
        return false;
    r[N]->head = 0;
    r[N]->tail = 0;
    return true;
}

void n64_memcpy(void* dst, const void* src, size_t size);

static void cb_write_data(int N, const void *src, size_t n) {
    __builtin_prefetch(src);
    uint32_t head = r[N]->head;
    uint32_t tail = r[N]->tail;
    uint32_t free = r[N]->cap - (head - tail);
    if (n > free)
        return;
    uint32_t idx = head & (r[N]->cap - 1);
    uint32_t first = MIN(n, r[N]->cap - idx);
    if (first)
        n64_memcpy(r[N]->buf + idx, src, first);
    if (n-first)
        n64_memcpy(r[N]->buf, (uint8_t*)src + first, n - first);
    r[N]->head = head + n;
}

static void cb_read_data(int N, void *dst, size_t n) {
    uint32_t tail = r[N]->tail;
    uint32_t idx = tail & (r[N]->cap - 1);
    __builtin_prefetch(r[N]->buf + idx);
    uint32_t head = r[N]->head;
    uint32_t avail = head - tail;
    if (n > avail)
        return;
    uint32_t first = MIN(n, r[N]->cap - idx);
    n64_memcpy(dst, r[N]->buf + idx, first);
    r[N]->tail = tail + n;
}
#else

#define CB_LEFT_ADDR   0x10000000
#define CB_RIGHT_ADDR  0x20000000
static void *const cb_buf[2] = {(void*)CB_LEFT_ADDR,(void*)CB_RIGHT_ADDR};

/* Macro for converting P1 address to physical memory address */
#define P1_TO_PHYSICAL(addr) ((uintptr_t)(addr) & MEM_AREA_CACHE_MASK)

//((uintptr_t)(addr) & ~MEM_AREA_P1_BASE)

static bool cb_init(int N, size_t capacity) {
    if (N == 0) {
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR           , P1_TO_PHYSICAL(cb_buf_internal[0])           , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + 4096    , P1_TO_PHYSICAL(cb_buf_internal[0]) + 4096    , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + (4096*2), P1_TO_PHYSICAL(cb_buf_internal[0]) + (4096*2), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + (4096*3), P1_TO_PHYSICAL(cb_buf_internal[0]) + (4096*3), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + (4096*4), P1_TO_PHYSICAL(cb_buf_internal[0])           , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + (4096*5), P1_TO_PHYSICAL(cb_buf_internal[0]) + 4096    , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + (4096*6), P1_TO_PHYSICAL(cb_buf_internal[0]) + (4096*2), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_LEFT_ADDR + (4096*7), P1_TO_PHYSICAL(cb_buf_internal[0]) + (4096*3), PAGE_SIZE_4K, MMU_ALL_RDWR, true);

        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR           , P1_TO_PHYSICAL(cb_buf_internal[1])           , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + 4096    , P1_TO_PHYSICAL(cb_buf_internal[1]) + 4096    , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + (4096*2), P1_TO_PHYSICAL(cb_buf_internal[1]) + (4096*2), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + (4096*3), P1_TO_PHYSICAL(cb_buf_internal[1]) + (4096*3), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + (4096*4), P1_TO_PHYSICAL(cb_buf_internal[1])           , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + (4096*5), P1_TO_PHYSICAL(cb_buf_internal[1]) + 4096    , PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + (4096*6), P1_TO_PHYSICAL(cb_buf_internal[1]) + (4096*2), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
        mmu_page_map_static((uintptr_t)CB_RIGHT_ADDR + (4096*7), P1_TO_PHYSICAL(cb_buf_internal[1]) + (4096*3), PAGE_SIZE_4K, MMU_ALL_RDWR, true);
    }
    // round capacity up to power of two
    r[N]->cap = 1u << (32 - __builtin_clz(capacity - 1));

    r[N]->buf = cb_buf[N];

    r[N]->head = 0;
    r[N]->tail = 0;
    return true;
}

void n64_memcpy(void* dst, const void* src, size_t size);

static void cb_write_data(int N, const void *src, size_t n) {
    __builtin_prefetch(src);
    uint32_t head = r[N]->head;
    uint32_t tail = r[N]->tail;
    uint32_t free = r[N]->cap - (head - tail);
    if (n > free)
        return;
    uint32_t idx = head & (r[N]->cap - 1);
    r[N]->head = head + n;
    n64_memcpy(r[N]->buf + idx, src, n);
}

static void cb_read_data(int N, void *dst, size_t n) {
    uint32_t tail = r[N]->tail;
    uint32_t idx = tail & (r[N]->cap - 1);
    __builtin_prefetch(r[N]->buf + idx);
    uint32_t head = r[N]->head;
    uint32_t avail = head - tail;
    if (n > avail)
        return;
    r[N]->tail = tail + n;
    n64_memcpy(dst, r[N]->buf + idx, n);
}

#endif

// --- KOS Stream Audio Callback (Consumer): Called by KOS when the AICA needs more data ---
#define NUM_BUFFER_BLOCKS (2)

void mute_stream(void) {
    snd_stream_volume(shnd, 0); // Set maximum volume
}

void unmute_stream(void) {
    snd_stream_volume(shnd, 160); // Set maximum volume
}

static size_t audio_cb(UNUSED snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    cb_read_data(0, (void*)l , req >> 1);
    cb_read_data(1, (void*)r , req >> 1);
    return req;
}

static bool audio_dc_init(void) {
    if (snd_stream_init()) {
        printf("AICA INIT FAILURE!\n");
        return false;
    }

    // without increasing scheduler frequency, things don't work nicely
    thd_set_hz(300);

    // --- Initial Pre-fill of Ring Buffer with Silence ---
    sq_clr(cb_buf_internal, sizeof(cb_buf_internal));

    if (!cb_init(0,RING_BUFFER_MAX_BYTES)) {
        printf("CB INIT FAILURE!\n");
        return false;
    }
    if (!cb_init(1,RING_BUFFER_MAX_BYTES)) {
        printf("CB INIT FAILURE!\n");
        return false;
    }

    printf("Dreamcast Audio: Initialized. %d Hz, ring buffer size: %u bytes per channel.\n",
           DC_AUDIO_FREQUENCY, (unsigned int)RING_BUFFER_MAX_BYTES);

    // Allocate the sound stream with KOS
    shnd = snd_stream_alloc(NULL, 8192);
    if (shnd == SND_STREAM_INVALID) {
        printf("SND: Stream allocation failure!\n");
        snd_stream_destroy(shnd);
        return false;
    }
    snd_stream_set_callback_direct(shnd, audio_cb);

    // Set maximum volume
    snd_stream_volume(shnd, 160); 

    printf("Sound init complete!\n");

    return true;
}

static int audio_dc_buffered(void) {
    return 1088;
}

static int audio_dc_get_desired_buffered(void) {
    return 1100;
}

static void audio_dc_play(uint8_t *bufL, uint8_t *bufR, size_t len) {
    cb_write_data(0, bufL, len >> 1);
    cb_write_data(1, bufR, len >> 1);

    if ((!audio_started)) {
        audio_started = true;
        snd_stream_start(shnd, DC_AUDIO_FREQUENCY, DC_STEREO_AUDIO);
    }
    
    if (audio_started) {
        snd_stream_poll(shnd);
    }
}

struct AudioAPI audio_dc = {
    audio_dc_init,
    audio_dc_buffered,
    audio_dc_get_desired_buffered,
    audio_dc_play
};
