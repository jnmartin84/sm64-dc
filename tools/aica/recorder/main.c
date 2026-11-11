// The AICA voice driver's log of each song and sound, on the build machine (Makefile here). What the
// console's audio thread does each vblank runs in step with the game's tick, and time is the
// vblanks gone by, at the rate the console measures: 4 sub-updates per vblank, 238.99 per second.
//
//   sm64_audio_rec <folder/> <songs, hex, comma-separated> <sounds, hex, comma-separated>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "sm64.h"
#include "types.h"

extern void audio_init(void);
extern void sound_init(void);
extern void create_next_audio_buffer(s16 *samplesL, s16 *samplesR, u32 num_samples);

static uint64_t sVblanks;
static s16 sBuffer[2][448 * 2 * 2 * 3];
static const char *sDir = "", *sSongs = "", *sSounds = "";

uint64_t timer_us_gettime64(void) {
    return sVblanks * 16736;
}

static void rec_wait_tick(void) {
    for (int i = 0; i < 2; i++) {
        create_next_audio_buffer(sBuffer[0], sBuffer[1], 448);
        sVblanks++;
    }
}
static uint64_t rec_now_us(void) { return timer_us_gettime64(); }
static const char *rec_dir(void) { return sDir; }
static int rec_seconds(int seq) { return seq >= 0 ? 420 : 20; }
static const char *rec_sequences(void) { return sSongs; }
static const char *rec_sounds(void) { return sSounds; }
#include "audio_record.inc.c"

// The voice driver's calls into KOS: no AICA behind them. Addresses are only handed back, so RAM
// never runs out and no note is dropped for want of it.
int snd_sh4_to_aica(void *packet, uint32_t size) {
    (void) packet;
    (void) size;
    return 0;
}
uint32_t snd_mem_malloc(size_t size) {
    static uint32_t next = 0x10000;
    uint32_t at = next;
    next += (uint32_t) ((size + 31) & ~(size_t) 31);
    return at;
}
void snd_mem_free(uint32_t addr) {
    (void) addr;
}
void spu_memload_sq(uintptr_t to, void *from, size_t length) {
    (void) to;
    (void) from;
    (void) length;
}
void spu_memread(void *to, uintptr_t from, size_t length) {
    (void) to;
    (void) from;
    (void) length;
}
const unsigned char gAicaAdpcmPool[1] = { 0 };

// What the audio code reads of the game, and its calls into the N64's OS (src/pc/ultra_reimplementation.c).
s16 gCurrLevelNum, gCurrAreaIndex, gMarioCurrentRoom;
struct MarioState gMarioStates[1];
void n64_memcpy(void *dst, const void *src, size_t size) {
    memcpy(dst, src, size);
}
void osCreateMesgQueue(UNUSED OSMesgQueue *mq, UNUSED OSMesg *msgBuf, UNUSED s32 count) {
}
s32 osRecvMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg *msg, UNUSED s32 flag) {
    return 0;
}
void osWritebackDCacheAll(void) {
}
void osWritebackDCache(UNUSED void *a, UNUSED size_t b) {
}
void osInvalDCache(UNUSED void *a, UNUSED size_t b) {
}
s32 osAiSetFrequency(u32 freq) {
    u32 a1;
    s32 a2;
    u32 D_8033491C;

#ifdef VERSION_EU
    D_8033491C = 0x02E6025C;
#else
    D_8033491C = 0x02E6D354;
#endif

    a1 = D_8033491C / (float) freq + .5f;

    if (a1 < 0x84) {
        return -1;
    }

    a2 = (a1 / 66) & 0xff;
    if (a2 > 16) {
        a2 = 16;
    }

    return D_8033491C / (s32) a1;
}

// The RSP audio microcode, which synthesis.c only calls without TARGET_DC.
#define NEVER(name) void name(void) { abort(); }
NEVER(aADPCMdecImpl) NEVER(aClearBufferImpl) NEVER(aDMEMMoveImpl) NEVER(aEnvMixerImpl) NEVER(aLoadADPCMImpl)
NEVER(aLoadBufferImpl) NEVER(aMixImpl) NEVER(aResampleImpl) NEVER(aSaveBuffer2Impl) NEVER(aSaveBufferImpl)
NEVER(aSetBufferImpl) NEVER(aSetLoopImpl) NEVER(aSetVolumeImpl)

int main(int argc, char **argv) {
    if (argc > 1) sDir = argv[1];
    if (argc > 2) sSongs = argv[2];
    if (argc > 3) sSounds = argv[3];
    audio_init();
    sound_init();
    audio_record_session();
    return 0;
}
