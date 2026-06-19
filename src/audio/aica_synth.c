/*
 * AICA hardware-mixed voice driver (Dreamcast) for SM64 (VERSION_US).
 * Drives AICA hardware channels from the live gNotes[] set each frame via the
 * KOS command queue; samples staged into ARAM from a linked offline-transcoded
 * Yamaha-ADPCM pool (gAicaAdpcmPool). Stock KOS firmware only (snd_sh4_to_aica
 * is the one non-public symbol, used for live channel UPDATE).
 *
 * Long samples (>65534, the AICA 16-bit channel length cap) are flagged stream=1
 * in the table and played by the SH4 PCM-ring streamer (see "Long-sample
 * streamer" below); everything else plays on a single hardware voice.
 */

#include "internal.h"
#include "load.h"
#include "data.h"
#include "aica_synth.h"

#ifdef TARGET_DC

#include <stdio.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <dc/sound/aica_comm.h>
#include <arch/timer.h>

extern int snd_sh4_to_aica(void* packet, uint32_t size);
extern u8 gSoundDataRaw[];   /* .tbl base; sample keys are offsets from here */

#define AICA_DEBUG 0
#define AICA_PAN_LOG 0      /* trace pan/vol per sub-update to diagnose fast pan/tremolo effects */
#define AICA_OCTAVE_LOG 1   /* log ADPCM samples pitched past ~+1 octave -> FORCE_PCM candidates */

#include "aica_sample_table.h"

static const unsigned char* sPoolBase = NULL;   /* linked gAicaAdpcmPool */
static u8* sTblBase = NULL;                      /* gSoundDataRaw */

#define NUM_AICA_CHANNELS 64
#define MAX_VOICES 64
#define ARAM_CACHE_ENTRIES 192
#define AICA_LEN_MAX 65534
#define WAVE_SAMPLE_COUNT 64
#define NUM_WAVEFORMS 4
#define NUM_WAVE_BANDS 4   /* US sampleCount 64/32/16/8 -> band 0..3 */

/* Synthetic waves are full-scale (+/-0x7FFF); two same-waveform voices summing on
   the AICA mix bus clip ("crunchy") where the N64's wider-accumulator mixer had
   headroom. Attenuate synth voices to leave room for a few to stack. Out of 256
   (256 = unity); tune on HW -- lower if still crunchy, raise if too quiet. */
#define SYNTH_VOL_SCALE 192
#define KEY_EMPTY 0xFFFFFFFFu
#define SYNTH_KEY(wf) (0x80000000u | (u32)(wf))

typedef struct {
    u32 key;
    u32 aram;
    u32 len;
    s32 refs;
    u32 lru;
} AramEntry;

static AramEntry sCache[ARAM_CACHE_ENTRIES];
static u32 sTick;
static s8 sChanFree[NUM_AICA_CHANNELS];
static s32 sChanFreeTop;

typedef struct {
    s8 channel;
    u8 active;
    u32 sampleKey;
    AramEntry* entry;
    s32 samplesLeft;   /* one-shot play-out countdown in samples; <0 = looped/untracked */
    u8  waveSc;        /* synth voice: sampleCount (64/32/16/8) of the loaded band buffer; 0 = non-synth */
    u32 sentFreq;      /* last freq/vol/pan pushed to the channel; gates redundant updates */
    u8  sentVol, sentPan;
} Voice;
static Voice sVoices[MAX_VOICES];
static u32 sWaveAram[NUM_WAVEFORMS][NUM_WAVE_BANDS];

/* ---- Long-sample streamer ----------------------------------------------------
   Samples longer than the AICA 16-bit channel cap (stream=1) can't be a single
   hardware voice. Each slot dedicates one AICA channel to a small PCM16 ring in
   ARAM, looped forever; the SH4 decodes the resident Yamaha-ADPCM into the ring
   ahead of a timer-estimated read pointer. PCM (not ADPCM) in the ring so refills
   never splice an ADPCM decoder mid-stream; for the one looped long sample we
   snapshot the decoder state at loopStart and restore it on wrap. */
#define MAX_STREAMS 2
#define STREAM_RING_SAMPLES 4096u   /* PCM16 ring per slot (8 KB ARAM) */
#define STREAM_GUARD 256u           /* keep the write head this far behind a full ring */

static const s32 ADPCM_DIFF[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15 };
static const s32 ADPCM_SCALE[8] = { 0xE6, 0xE6, 0xE6, 0xE6, 0x133, 0x199, 0x200, 0x266 };

typedef struct {
    s32 noteIndex;          /* owning gNotes[] index, -1 = free */
    s32 channel;
    u32 ringAram;
    u32 key;
    const u8* src;          /* resident Yamaha-ADPCM bytes */
    u32 nsamples;
    u32 loopStart, loopEnd;
    u8  loopFlag;
    s32 cur, quant;         /* decoder state */
    u32 srcPos;             /* next source sample to decode */
    s32 loopCur, loopQuant; u32 haveLoopSnap;   /* state captured at loopStart */
    u32 written;            /* total samples decoded into the ring */
    u32 freq;               /* fixed playback rate (Hz) */
    u64 startUs;            /* timer at playback start */
} Stream;
static Stream sStreams[MAX_STREAMS];

typedef struct {
    u32 base, type, length, loop, loopstart, loopend;
} Resolved;


static int sample_lookup(u32 key, const AicaSampleDesc** out) {
    s32 lo = 0, hi = AICA_SAMPLE_COUNT - 1;
    while (lo <= hi) {
        s32 mid = (lo + hi) >> 1;
        u32 k = gAicaSampleTable[mid].src_offset;
        if (k == key) { *out = &gAicaSampleTable[mid]; return 1; }
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static AramEntry* cache_find(u32 key) {
    s32 i;
    for (i = 0; i < ARAM_CACHE_ENTRIES; i++)
        if (sCache[i].key == key) return &sCache[i];
    return NULL;
}

static AramEntry* cache_acquire(u32 key, u32 pool_offset, u32 byte_len) {
    AramEntry* e = cache_find(key);
    s32 i;
    if (e) { e->refs++; e->lru = sTick; return e; }
    u32 aram = (u32)snd_mem_malloc(byte_len);
    if (aram == 0) {
        AramEntry* victim = NULL;
        for (i = 0; i < ARAM_CACHE_ENTRIES; i++)
            if (sCache[i].key != KEY_EMPTY && sCache[i].refs == 0)
                if (!victim || sCache[i].lru < victim->lru) victim = &sCache[i];
        if (victim) {
            snd_mem_free(victim->aram);
            victim->key = KEY_EMPTY; victim->aram = 0;
            aram = (u32)snd_mem_malloc(byte_len);
        }
        if (aram == 0) return NULL;
    }
    spu_memload_sq(aram, (void*)(sPoolBase + pool_offset), (byte_len + 31) & ~31);
    e = cache_find(KEY_EMPTY);
    if (!e) { snd_mem_free(aram); return NULL; }
    e->key = key; e->aram = aram; e->len = byte_len; e->refs = 1; e->lru = sTick;
    return e;
}

static void cache_release(AramEntry* e) { if (e && e->refs > 0) e->refs--; }

/* The KOS aica_freq firmware converts Hz -> AICA (octave, mantissa) via
   freq_lo = (freq<<10)/freq_base, which overflows its 10 bits when freq reaches
   2*freq_base (just below each 44100/2^n boundary, since freq_base is the
   integer-truncated 44100>>k). That drops an octave -> half-speed playback.
   Mirror the firmware's octave search and clamp to the last representable value
   so freq never lands in that gap. */
static u32 fix_aica_freq_gap(u32 freq) {
    u32 base = 5644800;   /* 44100 * 128 */
    s32 hi = 7;
    while (freq < base && hi > -8) { base >>= 1; hi--; }
    if (base != 0 && freq >= 2 * base) freq = 2 * base - 1;
    return freq;
}

/* note->frequency is the resampling ratio (sampleRate / gAiFrequency), already
   scaled by 32000/gAiFrequency in playback.c, so freq * gAiFrequency yields the
   game's intended 32000-referenced playback rate regardless of the session AI
   rate. Use the live gAiFrequency global (OoT lesson: read the runtime field). */
/* Normalize note->sampleCount to the canonical band value the engine emits. */
static u32 norm_sc(u32 sampleCount) {
    switch (sampleCount) {
        case 32: return 32;
        case 16: return 16;
        case 8:  return 8;
        default: return 64;   /* 64, and any unexpected value */
    }
}

/* Map the US engine's note->sampleCount (64/32/16/8) to a wave-buffer band. */
static u32 wave_band(u32 sampleCount) {
    switch (sampleCount) {
        case 32: return 1;
        case 16: return 2;
        case 8:  return 3;
        default: return 0;   /* 64, and any unexpected value */
    }
}

/* note->frequency*gAiFrequency is the intended playback rate when the loaded
   wave buffer's band matches the engine's current note->sampleCount. A sustained
   synth note can drift across a band boundary (build_synthetic_wave re-runs and
   nudges freqScale), but we keep its original buffer to avoid a restart-from-0
   stutter; loadedSc carries that buffer's sampleCount so we rescale the rate by
   loadedSc/currentSc and the pitch stays continuous across the drift. loadedSc=0
   (non-synth, or matched band) means no rescale. */
static u32 calc_freq(struct Note* note, u32 loadedSc) {
    f32 f = note->frequency * (f32)gAiFrequency;
    s32 fi;
    if (loadedSc) {
        u32 cur = norm_sc(note->sampleCount);
        if (cur && cur != loadedSc) f = f * (f32)loadedSc / (f32)cur;
    }
    fi = (s32)f;
    if (fi < 1) fi = 1;
    return fix_aica_freq_gap((u32)fi);
}

static u32 calc_vol(struct Note* note) {
    /* US targetVol is (velocity*vol) & 0x7F00, range 0..0x7F00 (synthesis.c) --
       8x the EU/MK64 0..0xFFF scale. Map linearly to 0..255 with >>7 (0x7F00>>7
       = 254); the old >>4 saturated nearly every note to max. */
    u32 l = note->targetVolLeft, r = note->targetVolRight;
    u32 m = (l > r) ? l : r;
    u32 v = m >> 7;
    if (v > 255) v = 255;
    /* Headroom for full-scale synth waves summing without clipping (see above). */
    if (note->sound == NULL) v = (v * SYNTH_VOL_SCALE) >> 8;
    return v;
}

/* AICA pan (DIPAN) attenuates the opposite channel in ~3 dB steps. KOS
   calc_aica_pan maps 0..255 -> a 0..15 step amount: left -> 127 - amount*8,
   right -> 128 + amount*8. Convert the L/R balance to dB and pick the matching
   step count (no float/libm) instead of a linear formula that over-attenuates. */
static u32 calc_pan(struct Note* note) {
    s32 l = note->targetVolLeft, r = note->targetVolRight;
    s32 hi, lo, amount;
    int left;

    if (l == r) return 128;
    if (l > r) { hi = l; lo = r; left = 1; } else { hi = r; lo = l; left = 0; }

    if (lo <= 0) {
        amount = 15;
    } else {
        amount = 0;
        while (amount < 15 && (s64)lo * 1000 < (s64)hi * 708) {
            lo = (s32)(((s64)lo * 1000) / 708);   /* step up by ~3 dB */
            amount++;
        }
    }
    return left ? (u32)(127 - amount * 8) : (u32)(128 + amount * 8);
}

/* Returns: 0 = skip, 1 = normal hardware voice (res/entry filled),
   2 = streamed long sample (*outDesc + *outKey filled, no entry). */
static int resolve(struct Note* note, u32* outKey, Resolved* res, AramEntry** outEntry,
                   const AicaSampleDesc** outDesc) {
    *outEntry = NULL;
    *outDesc = NULL;

    if (note->sound == NULL) {
        /* synthetic wave: per-band decimated copy of gWaveSamples[instOrWave-0x80],
           64 s16 samples, looped. Folding the band into the key makes a band change
           (rare: pitch bend/vibrato crossing a 2x boundary) re-trigger onto the
           correct buffer. */
        u32 wf = (u32)(note->instOrWave - 0x80);
        u32 band;
        if (wf >= NUM_WAVEFORMS) return 0;
        band = wave_band(note->sampleCount);
        *outKey = SYNTH_KEY(wf);
        res->base = sWaveAram[wf][band];
        res->type = AICA_SM_16BIT;
        res->length = WAVE_SAMPLE_COUNT;
        res->loop = 1; res->loopstart = 0; res->loopend = WAVE_SAMPLE_COUNT;
        return 1;
    }

    if (sPoolBase == NULL) return 0;
    {
        struct AudioBankSample* s = note->sound->sample;
        const AicaSampleDesc* d;
        u32 key;
        AramEntry* e;
        if (s == NULL || s->sampleAddr == NULL) return 0;
        key = (u32)s->sampleAddr - (u32)sTblBase;
        if (!sample_lookup(key, &d)) return 0;
        if (d->stream) { *outDesc = d; *outKey = key; return 2; }   /* SH4-streamed */
        e = cache_acquire(key, d->pool_offset, d->byte_len);
        if (!e) return 0;
        *outEntry = e; *outKey = key;
        res->base = e->aram;
        res->type = d->fmt;   /* AICA_SM_16BIT/8BIT/ADPCM, chosen offline by SNR */
        res->length = d->nsamples > AICA_LEN_MAX ? AICA_LEN_MAX : d->nsamples;
        res->loop = d->loop_flag;
        res->loopstart = d->loop_start;
        res->loopend = d->loop_end ? d->loop_end : res->length;
        return 1;
    }
}

static void chan_start(s32 ch, const Resolved* r, u32 freq, u32 vol, u32 pan) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_START;
    chan->base = r->base; chan->type = r->type; chan->length = r->length;
    chan->loop = r->loop; chan->loopstart = r->loopstart; chan->loopend = r->loopend;
    chan->freq = freq; chan->vol = vol; chan->pan = pan;
    snd_sh4_to_aica(tmp, cmd->size);
}

static void chan_update(s32 ch, u32 freq, u32 vol, u32 pan) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ | AICA_CH_UPDATE_SET_VOL | AICA_CH_UPDATE_SET_PAN;
    chan->freq = freq; chan->vol = vol; chan->pan = pan;
    snd_sh4_to_aica(tmp, cmd->size);
}

static void chan_stop(s32 ch) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_STOP;
    snd_sh4_to_aica(tmp, cmd->size);
}

static s32 chan_alloc(void) { return sChanFreeTop ? sChanFree[--sChanFreeTop] : -1; }
static void chan_release(s32 ch) { if (sChanFreeTop < NUM_AICA_CHANNELS) sChanFree[sChanFreeTop++] = (s8)ch; }

static void voice_stop(s32 i) {
    Voice* v = &sVoices[i];
    if (v->channel >= 0) { chan_stop(v->channel); chan_release(v->channel); }
    cache_release(v->entry);
    v->channel = -1; v->active = 0; v->entry = NULL; v->sampleKey = KEY_EMPTY;
    v->samplesLeft = -1; v->waveSc = 0; v->sentFreq = 0; v->sentVol = 0; v->sentPan = 0;
}

/* Decode the next source sample, advancing decoder + loop state. Returns PCM16.
   One-shot past end returns silence. */
static s16 stream_decode_one(Stream* s) {
    s32 code, m, delta, c, q;
    u8 byte;
    if (!s->loopFlag && s->srcPos >= s->nsamples) return 0;   /* drained */
    if (s->loopFlag && !s->haveLoopSnap && s->srcPos == s->loopStart) {
        s->loopCur = s->cur; s->loopQuant = s->quant; s->haveLoopSnap = 1;
    }
    byte = s->src[s->srcPos >> 1];
    code = (s->srcPos & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
    m = code & 7;
    delta = (s->quant * ADPCM_DIFF[code]) / 8;     /* C trunc toward zero */
    c = s->cur + delta;
    if (c < -32768) c = -32768; else if (c > 32767) c = 32767;
    s->cur = c;
    q = (s->quant * ADPCM_SCALE[m]) >> 8;
    if (q < 127) q = 127; else if (q > 24576) q = 24576;
    s->quant = q;
    s->srcPos++;
    if (s->loopFlag && s->srcPos >= s->loopEnd) {
        s->cur = s->loopCur; s->quant = s->loopQuant; s->srcPos = s->loopStart;
    }
    return (s16)c;
}

/* Decode 16-sample blocks into the ring until the write head reaches `target`
   (clamped to one ring-length ahead of the read estimate). 16-sample alignment
   keeps spu_memload_sq dest/size 32-byte aligned. */
static void stream_fill(Stream* s, u32 target) {
    u32 cap = s->written + 4 * STREAM_RING_SAMPLES;   /* per-call safety bound */
    while (s->written < target && s->written < cap) {
        s16 tmp[16];
        u32 off, k;
        for (k = 0; k < 16; k++) tmp[k] = stream_decode_one(s);
        off = s->written & (STREAM_RING_SAMPLES - 1);
        spu_memload_sq(s->ringAram + off * 2, tmp, 32);
        s->written += 16;
    }
}

static void stream_free(Stream* s) {
    if (s->channel >= 0) { chan_stop(s->channel); chan_release(s->channel); }
    s->channel = -1; s->noteIndex = -1; s->key = KEY_EMPTY;
}

static Stream* stream_for_note(s32 noteIndex) {
    s32 i;
    for (i = 0; i < MAX_STREAMS; i++)
        if (sStreams[i].noteIndex == noteIndex) return &sStreams[i];
    return NULL;
}

static void stream_start(s32 noteIndex, const AicaSampleDesc* d, u32 freq, u32 vol, u32 pan) {
    Stream* s = NULL;
    s32 i, ch;
    for (i = 0; i < MAX_STREAMS; i++) if (sStreams[i].noteIndex < 0) { s = &sStreams[i]; break; }
    if (!s) return;                       /* all slots busy: drop (rare) */
    ch = chan_alloc();
    if (ch < 0) return;

    s->noteIndex = noteIndex; s->channel = ch; s->key = d->src_offset;
    s->src = sPoolBase + d->pool_offset;
    s->nsamples = d->nsamples;
    s->loopFlag = d->loop_flag;
    s->loopStart = d->loop_start;
    s->loopEnd = d->loop_end ? d->loop_end : d->nsamples;
    s->cur = 0; s->quant = 127; s->srcPos = 0; s->haveLoopSnap = 0;
    s->written = 0; s->freq = freq;

    stream_fill(s, STREAM_RING_SAMPLES);  /* pre-fill the whole ring */

    {
        AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
        cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
        chan->cmd = AICA_CH_CMD_START;
        chan->base = s->ringAram; chan->type = AICA_SM_16BIT; chan->length = STREAM_RING_SAMPLES;
        chan->loop = 1; chan->loopstart = 0; chan->loopend = STREAM_RING_SAMPLES;
        chan->freq = freq; chan->vol = vol; chan->pan = pan;
        snd_sh4_to_aica(tmp, cmd->size);
    }
    s->startUs = timer_us_gettime64();      /* read estimate is zero at playback start */
}

/* Service one active stream: advance the read estimate, refill, update vol/pan.
   Returns 0 when a one-shot has fully played out (caller frees + finishes note). */
static int stream_service(Stream* s, u32 vol, u32 pan) {
    u64 now = timer_us_gettime64();
    u32 consumed = (u32)(((u64)s->freq * (now - s->startUs)) / 1000000ULL);
    u32 target;

    if (!s->loopFlag && consumed >= s->nsamples) return 0;   /* done */

    target = (consumed + STREAM_RING_SAMPLES - STREAM_GUARD) & ~15u;
    if (target > consumed + STREAM_RING_SAMPLES) target = consumed + STREAM_RING_SAMPLES;
    stream_fill(s, target);

    chan_update(s->channel, s->freq, vol, pan);
    return 1;
}

void AicaSynth_Init(void) {
    s32 i, ch; u32 w;

    for (i = 0; i < ARAM_CACHE_ENTRIES; i++) { sCache[i].key = KEY_EMPTY; sCache[i].aram = 0; sCache[i].refs = 0; }
    for (i = 0; i < MAX_VOICES; i++) { sVoices[i].channel = -1; sVoices[i].active = 0; sVoices[i].entry = NULL; sVoices[i].sampleKey = KEY_EMPTY; sVoices[i].samplesLeft = -1; sVoices[i].waveSc = 0; sVoices[i].sentFreq = 0; sVoices[i].sentVol = 0; sVoices[i].sentPan = 0; }
    sChanFreeTop = 0;
    for (ch = NUM_AICA_CHANNELS - 1; ch >= 0; ch--) sChanFree[sChanFreeTop++] = (s8)ch;

    sPoolBase = gAicaAdpcmPool;          /* linked, resident */
    sTblBase = (u8*)gSoundDataRaw;

    /* Build the four per-band wave buffers the US build_synthetic_wave produces
       (sampleCount 64/32/16/8): decimate one cycle of the raw 64-sample wave by
       stepSize, then repeat it to fill a 64-sample loop buffer. Uploading these
       (rather than looping the raw wave at a scaled-up rate) keeps the AICA
       playback rate near the output rate, so high notes don't alias, and the
       timbre matches the N64 software synth. */
    {
        static const u32 kSampleCount[NUM_WAVE_BANDS] = { 64, 32, 16, 8 };
        u32 band;
        for (w = 0; w < NUM_WAVEFORMS; w++) {
            for (band = 0; band < NUM_WAVE_BANDS; band++) {
                s16 buf[WAVE_SAMPLE_COUNT] __attribute__((aligned(32)));
                u32 sc = kSampleCount[band];
                u32 step = WAVE_SAMPLE_COUNT / sc;   /* 1,2,4,8 */
                u32 aram, i, pos, off, j;
                for (i = 0, pos = 0; pos < WAVE_SAMPLE_COUNT; pos += step) buf[i++] = gWaveSamples[w][pos];
                for (off = sc; off < WAVE_SAMPLE_COUNT; off += sc)
                    for (j = 0; j < sc; j++) buf[off + j] = buf[j];
                aram = (u32)snd_mem_malloc(WAVE_SAMPLE_COUNT * sizeof(s16));
                spu_memload_sq(aram, buf, WAVE_SAMPLE_COUNT * sizeof(s16));
                sWaveAram[w][band] = aram;
            }
        }
    }

    for (i = 0; i < MAX_STREAMS; i++) {
        sStreams[i].noteIndex = -1;
        sStreams[i].channel = -1;
        sStreams[i].key = KEY_EMPTY;
        sStreams[i].ringAram = (u32)snd_mem_malloc(STREAM_RING_SAMPLES * sizeof(s16));
    }

    printf("AicaSynth_Init: pool=%p tbl=%p samples=%d\n",
           (void*)sPoolBase, (void*)sTblBase, (int)AICA_SAMPLE_COUNT);

#if AICA_DEBUG
    /* ARAM round-trip self-test: upload sample[0]'s ADPCM, read it back, and
       confirm the bytes the AICA will decode match the linked pool. */
    {
        const AicaSampleDesc* d = &gAicaSampleTable[0];
        u32 aram = (u32)snd_mem_malloc(d->byte_len);
        unsigned char chk[64];
        s32 k, bad = 0;
        spu_memload_sq(aram, (void*)(sPoolBase + d->pool_offset), (d->byte_len + 31) & ~31);
        spu_memread(chk, aram, 64);
        for (k = 0; k < 64; k++)
            if (chk[k] != sPoolBase[d->pool_offset + k]) bad++;
        printf("AICA selftest: key=%X aram=%X len=%u mismatch=%d/64 src=%02X%02X%02X%02X aram=%02X%02X%02X%02X\n",
               (unsigned)d->src_offset, (unsigned)aram, (unsigned)d->byte_len, (int)bad,
               sPoolBase[d->pool_offset], sPoolBase[d->pool_offset+1],
               sPoolBase[d->pool_offset+2], sPoolBase[d->pool_offset+3],
               chk[0], chk[1], chk[2], chk[3]);
        snd_mem_free(aram);
    }
#endif
}

/* Re-push freq/vol/pan for already-active voices. Called once per sequence
   sub-update (gAudioUpdatesPerFrame times/frame) so fast pan/tremolo/vibrato
   keep the sub-frame resolution the N64 render had -- AicaSynth_Update alone
   runs once/frame and would alias them to a single snapshot held flat. Voice
   lifecycle (start/stop/retrigger, one-shot countdown) stays in AicaSynth_Update.
   Gated on change so static notes generate no extra command-queue traffic. */
void AicaSynth_RefreshActive(void) {
    s32 numNotes = gMaxSimultaneousNotes;
    s32 i;
#if AICA_PAN_LOG
    static u32 sRefreshTick = 0;
    sRefreshTick++;
#endif

    if (sPoolBase == NULL) return;
    if (numNotes > MAX_VOICES) numNotes = MAX_VOICES;

    for (i = 0; i < numNotes; i++) {
        Voice* v = &sVoices[i];
        struct Note* note = &gNotes[i];
        u32 freq, vol, pan;

        if (!v->active || v->channel < 0) continue;     /* streams + idle slots */
        if (!note->enabled || note->finished) continue; /* let AicaSynth_Update stop it */

        if (note->sound == NULL)
            freq = calc_freq(note, v->waveSc ? v->waveSc : norm_sc(note->sampleCount));
        else
            freq = calc_freq(note, 0);
        vol = calc_vol(note);
        pan = calc_pan(note);

        if (freq != v->sentFreq || (u8)vol != v->sentVol || (u8)pan != v->sentPan) {
            chan_update(v->channel, freq, vol, pan);
            v->sentFreq = freq; v->sentVol = (u8)vol; v->sentPan = (u8)pan;
#if AICA_PAN_LOG
            {
                static u32 lines = 0;
                if (lines < 4000) {
                    printf("PAN t=%u v=%d L=%d R=%d pan=%u vol=%u\n",
                           (unsigned)sRefreshTick, (int)i,
                           (int)note->targetVolLeft, (int)note->targetVolRight,
                           (unsigned)pan, (unsigned)vol);
                    lines++;
                }
            }
#endif
        }
    }
}

void AicaSynth_Update(void) {
    s32 numNotes = gMaxSimultaneousNotes;
    s32 i;

    if (sPoolBase == NULL) return;   /* not initialized yet */
    sTick++;
    if (numNotes > MAX_VOICES) numNotes = MAX_VOICES;

    for (i = 0; i < numNotes; i++) {
        struct Note* note = &gNotes[i];
        Voice* v = &sVoices[i];
        u32 key, freq, vol, pan;
        Resolved res;
        AramEntry* entry;
        const AicaSampleDesc* desc;
        Stream* st;
        int retrigger, rc;

        if (!note->enabled || note->finished) {
            if (v->active) voice_stop(i);
            st = stream_for_note(i); if (st) stream_free(st);
            continue;
        }
        rc = resolve(note, &key, &res, &entry, &desc);
        if (rc == 0) {
            if (v->active) voice_stop(i);
            st = stream_for_note(i); if (st) stream_free(st);
            continue;
        }

        vol = calc_vol(note);
        pan = calc_pan(note);

        if (rc == 2) {
            /* Streamed long sample: dedicated channel + SH4-fed PCM ring. */
            freq = calc_freq(note, 0);
            if (v->active) voice_stop(i);          /* this note is never a normal voice */
            st = stream_for_note(i);
            if (st && (st->key != desc->src_offset || note->needsInit)) { stream_free(st); st = NULL; }
            note->needsInit = FALSE;
            if (!st) stream_start(i, desc, freq, vol, pan);
            else if (!stream_service(st, vol, pan)) { stream_free(st); note->finished = TRUE; }
            continue;
        }

        retrigger = (!v->active) || (v->sampleKey != key) || note->needsInit;
        /* Consume needsInit ourselves -- the render path that normally cleared
           it (synthesis_process_notes) is replaced. Otherwise every note looks
           freshly-triggered each frame and the voice restarts ~60x/sec, so the
           sample never plays past its attack -> "robot noise". */
        note->needsInit = FALSE;

        /* For a synth voice, freq is rescaled to the band buffer actually loaded:
           on retrigger that's the current band (no rescale); otherwise the buffer
           we started with (v->waveSc), so a band drift adjusts pitch instead of
           restarting the channel. Non-synth voices pass loadedSc=0. */
        if (note->sound == NULL)
            freq = calc_freq(note, retrigger ? norm_sc(note->sampleCount)
                                             : (v->waveSc ? v->waveSc : norm_sc(note->sampleCount)));
        else
            freq = calc_freq(note, 0);

        if (retrigger) {
            s32 chn;
            if (v->active) voice_stop(i);
            chn = chan_alloc();
            if (chn < 0) { cache_release(entry); continue; }
            v->channel = chn; v->active = 1; v->sampleKey = key; v->entry = entry;
            v->waveSc = (note->sound == NULL) ? (u8)norm_sc(note->sampleCount) : 0;
            v->samplesLeft = res.loop ? -1 : (s32)res.length;
            chan_start(chn, &res, freq, vol, pan);
            v->sentFreq = freq; v->sentVol = (u8)vol; v->sentPan = (u8)pan;
#if AICA_OCTAVE_LOG
            /* Find ADPCM samples the game pitches past ~+1 octave (note->frequency
               > ~2). Those pitch-clamp on AICA ADPCM and need FORCE_PCM. nf=1.89
               was fine, nf=4.0 (BEC60) was broken; log from 1.5 to see the spread.
               One print per sample per new high; capped so it can't flood. */
            if (note->sound != NULL && res.type == AICA_SM_ADPCM && note->frequency > 2.0f) {
                static u32 oKey[128]; static float oMax[128]; static s32 oN = 0;
                s32 j, hit = -1;
                for (j = 0; j < oN; j++) if (oKey[j] == key) { hit = j; break; }
                if (hit < 0 && oN < 128) { hit = oN++; oKey[hit] = key; oMax[hit] = 0.0f; }
                if (hit >= 0 && note->frequency > oMax[hit]) {
                    oMax[hit] = note->frequency;
                    printf("OCTAVE key=%X maxnf=%f\n", (unsigned)key, (double)note->frequency);
                }
            }
#endif
#if AICA_DEBUG
            {
                static s32 dbgStarts = 0;
                if (dbgStarts < 4096) {
                    printf("AICAstart ch=%d key=%X synth=%d type=%u base=%X len=%u loop=%u ls=%u le=%u freq=%u vol=%u pan=%u nf=%f\n",
                           (int)chn, (unsigned)key, (note->sound == NULL),
                           (unsigned)res.type, (unsigned)res.base, (unsigned)res.length,
                           (unsigned)res.loop, (unsigned)res.loopstart, (unsigned)res.loopend,
                           (unsigned)freq, (unsigned)vol, (unsigned)pan, (double)note->frequency);
                    dbgStarts++;
                }
            }
#endif
        } else {
            if (entry) cache_release(entry);
            if (freq != v->sentFreq || (u8)vol != v->sentVol || (u8)pan != v->sentPan) {
                chan_update(v->channel, freq, vol, pan);
                v->sentFreq = freq; v->sentVol = (u8)vol; v->sentPan = (u8)pan;
            }
        }

        /* One-shot play-out: AicaSynth_Update runs ~once per video frame, so the
           voice consumes ~freq/60 samples per call. When a non-looping sample
           has played its length, tell the engine the note is finished so it gets
           released promptly (otherwise it lingers and overlaps a re-trigger of
           the same sound -> audible doubling). The next frame's gate stops the
           now-silent channel. */
        if (v->samplesLeft > 0) {
            v->samplesLeft -= (s32)(freq / 60u);
            if (v->samplesLeft <= 0) {
                v->samplesLeft = -1;
                note->finished = TRUE;
            }
        }
    }
}

#else
void AicaSynth_Init(void) {}
void AicaSynth_Update(void) {}
void AicaSynth_RefreshActive(void) {}
#endif
