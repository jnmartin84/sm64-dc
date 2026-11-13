/* The game's audio through AICAflow.
 *
 * src/audio/external.c keeps the game's logic: the music queue, the volumes in the sequence
 * players, and the sound banks with their priorities and one channel each. Where it would start
 * a sequence or run a sound script, it calls in here. Nothing is sequenced on the SH4: a song or
 * sound is one AFB (its samples)
 * and one AFX (its control flow), built offline from a recording of the
 * original driver (tools/aica/sm64_audio_assets.sh). A song loops by itself
 * (AFX LOOP); a sound ends by itself.
 *
 * Assets are read from DC_AICAFLOW_ROOT when first needed: seqXX.afb/.afx for
 * sequence XX (hex), sfx_XXXXXXXX.afb/.afx for a sound's bank and id bits
 * (soundBits & 0xF0FF0000). Missing ones are remembered and stay silent.
 * Loading is synchronous; a planned per-area residency comes later.
 *
 * Reverb is the N64 mixer's: one feedback delay line, its length and feedback from the audio
 * session preset (sound_reset), as the scene's DSP program (AICAflow's delay prefab). Songs carry
 * each voice's send in their flows (from the recording); sounds are built with a full send that
 * is scaled here, through lane 0, to what get_sound_reverb would give the sound now. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include <aicaflow/host.h>
#include <aicaflow/bank.h>
#include <aicaflow/codec.h>

#include "sm64_aicaflow.h"
#include "sm64_sfx_reverb.h"

/* stop_inst sends each STOP once, which loses stops on a firmware before ABI 12. */
_Static_assert(AFX_ABI_VERSION >= 12, "third_party/aicaflow must have firmware ABI 12 or later");
extern s16 gCurrLevelNum;

#ifndef DC_AICAFLOW_ROOT
#define DC_AICAFLOW_ROOT "/pc/afx"
#endif
#define MUSIC_PLAYERS 2
#define SFX_ASSETS 320
#define SONG_CACHE 16
#define SFX_CHANNELS 10   /* SOUND_BANK_COUNT, one channel each (sMaxChannelsForSoundBank) */
#define MENU_BANK 7
#define SEQ_CREDITS 0x1A
/* gCurrLevelNum of LEVEL_BOWSER_3 and LEVEL_CASTLE_GROUNDS (levels/level_defines.h order). */
#define LEVEL_BOWSER_3_NUM 34
#define LEVEL_CASTLE_GROUNDS_NUM 16
#define RETIRING 16

extern const unsigned char gAicaflowFirmware[];
extern const unsigned int gAicaflowFirmwareSize;

typedef struct {
    afx_bank_t bank;
    afx_asset_t flow;
    u32 id;
    u8 resident;   /* loaded from a scene pack: kept until its scene ends, not retired */
    u8 core;       /* from scene_core: kept for the whole run */
    u8 lanes;      /* the flow's lane count: afx_instance_lanes_set drops a mask beyond it */
    u8 holds_core; /* its flow plays from sCoreBank, alone or beside the level's (pack bank 1 or 2) */
    afx_bank_t *shared;   /* a version 2 pack's scene bank, which it does not own; else bank */
} Asset;

/* Version 2 and 3 packs (tools/aica/sm64_scene_banks.py) bring one sample bank each that their
 * flows bind to: scene_core's for the run, the level's until its last flow is freed. In version
 * 3 a level flow can play from both, the level's bank in slot 0 and scene_core's in slot 1. */
/* PACK_BANK_FILES: an asset the level plays that its pack has no room for (tools/aica/sm64_scene_banks.py),
 * read into main RAM from its own files when the level loads. */
enum { PACK_BANK_OWN, PACK_BANK_CORE, PACK_BANK_BOTH, PACK_BANK_FILES };
static afx_bank_t sCoreBank, sLevelBank;
static u8 sCoreBankUp, sLevelBankUp, sLevelBankDying;

/* The ended level's bank goes once no flow binds to it (retired flows may still, briefly). */
static void level_bank_release(void) {
    if (sLevelBankDying && afx_bank_release(&sLevelBank) == 0) {
        sLevelBankUp = sLevelBankDying = 0;
    }
}

typedef struct {
    Asset asset;             /* the loaded song; id = sequence + 1, 0 = none */
    afx_instance_t inst;
    f32 gain;                /* from external.c, over the volume the song was recorded at */
    u8 sentGain;
    u8 sentLane[16];
    u8 seq;
    u8 paused;               /* AFX_CMD_PAUSE sent: the song waits for the pause menu to close */
    u32 pausedAt;            /* AICA timer tick then */
} Music;

static Music sMusic[MUSIC_PLAYERS];
static Asset sSfx[SFX_ASSETS];
/* Songs from scene packs, kept loaded so a song that comes round again (the title's) is not
 * read from /pc again; id = sequence + 1. */
static Asset sSongs[SONG_CACHE];
static s16 sSceneLevel = -1;
static u8 sCorePackTried;
static u8 sPackAbsent[128];   /* per level: it has no pack (not looked for again) */
/* A sound channel: what update_game_sound last started on it. */
typedef struct {
    afx_instance_t inst;
    u32 key;
    u8 gain;     /* last sent */
    s8 pan;      /* last sent lane pan offset */
    s8 pitch;    /* last sent lane pitch offset */
    u8 script;   /* the sound script's reverb (sSfxScriptReverb) */
    u8 send;     /* last sent lane DSP send */
    f32 fade, fadeStep;   /* func_803210D4's fade at a level's end, 1 down to 0 */
} SfxChannel;
static SfxChannel sChannel[SFX_CHANNELS];
static u8 sMuted;
static struct { Asset asset; afx_instance_t inst; } sRetiring[RETIRING];
#define DBG(...) ((void) 0)
static u8 sMissingSeq[128];
static u32 sMissingSfx[256];
static u32 sMissingSfxCount;
static int sReady;

/* The running level's pack, whole, in main RAM (pack_load); NULL if it did not fit there. */
static u8 *sLevelPack;
static u32 sLevelPackBytes;

static void level_pack_drop(void) {
    free(sLevelPack);
    sLevelPack = NULL;
    sLevelPackBytes = 0;
}

static int read_file(const char *path, void **out, u32 *bytes) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = size > 0 ? memalign(32, (size_t) size) : NULL;
    if (data == NULL || fread(data, 1, (size_t) size, f) != (size_t) size) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    *out = data;
    *bytes = (u32) size;
    return 0;
}

/* bank fills the flow's bank slot 0, and also (unless NULL) slot 1 (AFX_FLAG_BANKS). */
static int flow_upload(Asset *a, afx_bank_t *bank, afx_bank_t *also, const void *data, u32 bytes) {
    afx_file_header_t h;
    const afx_bank_t *const banks[2] = { bank, also };
    int result = afx_flow_upload(banks, also ? 2 : 1, data, bytes, &a->flow);
    a->lanes = 0;
    if (!result && afx_file_validate(data, bytes, &h) == AFX_OK && (h.flags & AFX_FLAG_LANES)) {
        const u8 *map = (const u8 *) data + h.image_offset + h.setup_count * AFX_SETUP_BYTES;
        for (u32 i = 0; i < h.required_channels; i++) {
            if (map[i] >= a->lanes) a->lanes = map[i] + 1;
        }
    }
    return result;
}

/* Assets a scene will load from their own files, read into main RAM ahead of time at a load point,
 * so that when they play they go to AICA RAM from memory instead of from the disc: those a level's
 * pack has no room for (kept until the scene ends, as AICA RAM may make them leave and come back),
 * and the credits' (see sm64_afx_scene_prepare: their song, in no pack, and the sounds they play
 * once the core pack has gone for it; each copy goes once it has been uploaded). */
#define PREFETCH_MAX 16
static struct {
    char stem[24];
    void *afb, *afx;
    u32 afb_bytes, afx_bytes;
    u8 keep;
} sPrefetch[PREFETCH_MAX];

static void prefetch_drop(int i) {
    free(sPrefetch[i].afb);
    free(sPrefetch[i].afx);
    memset(&sPrefetch[i], 0, sizeof(sPrefetch[i]));
}

static int prefetch_find(const char *stem) {
    for (int i = 0; i < PREFETCH_MAX; i++)
        if (sPrefetch[i].stem[0] && !strcmp(sPrefetch[i].stem, stem)) return i;
    return -1;
}

static void prefetch(const char *stem, u8 keep) {
    char path[128];
    int i = 0;
    if (prefetch_find(stem) >= 0) return;
    while (i < PREFETCH_MAX && sPrefetch[i].stem[0]) i++;
    if (i == PREFETCH_MAX) return;
    snprintf(path, sizeof(path), "%s/%s.afb", DC_AICAFLOW_ROOT, stem);
    int ok = read_file(path, &sPrefetch[i].afb, &sPrefetch[i].afb_bytes) == 0;
    snprintf(path, sizeof(path), "%s/%s.afx", DC_AICAFLOW_ROOT, stem);
    ok = ok && read_file(path, &sPrefetch[i].afx, &sPrefetch[i].afx_bytes) == 0;
    if (ok) {
        snprintf(sPrefetch[i].stem, sizeof(sPrefetch[i].stem), "%s", stem);
        sPrefetch[i].keep = keep;
    } else {
        prefetch_drop(i);   /* no room in main RAM: it loads from its files when it plays */
    }
}

static int asset_load_files(Asset *a, const char *stem, u32 id, u32 *bytes) {
    char path[128];
    void *flow = NULL;
    memset(a, 0, sizeof(*a));
    int p = prefetch_find(stem);
    if (p >= 0) {
        int result = afx_bank_load_memory(&a->bank, sPrefetch[p].afb, sPrefetch[p].afb_bytes);
        *bytes = sPrefetch[p].afx_bytes;
        if (!result && (result = flow_upload(a, &a->bank, NULL, sPrefetch[p].afx, sPrefetch[p].afx_bytes)) != 0)
            afx_bank_release(&a->bank);
        if (!result) {
            if (!sPrefetch[p].keep) prefetch_drop(p);
            a->id = id;
            return 0;
        }
        memset(a, 0, sizeof(*a));
        if (result == -AFX_NO_AICA_RAM) return result;   /* song_fetch makes room and tries again */
        prefetch_drop(p);
    }
    snprintf(path, sizeof(path), "%s/%s.afb", DC_AICAFLOW_ROOT, stem);
    int result = afx_bank_load_file(&a->bank, path);
    if (result) return result;
    snprintf(path, sizeof(path), "%s/%s.afx", DC_AICAFLOW_ROOT, stem);
    result = read_file(path, &flow, bytes) ? -1 : flow_upload(a, &a->bank, NULL, flow, *bytes);
    free(flow);
    if (result) {
        afx_bank_release(&a->bank);
        memset(a, 0, sizeof(*a));
        printf("AICAFLOW: %s: flow not loaded (%d)\n", stem, result);
        return result ? result : -1;
    }
    a->id = id;
    return 0;
}

/* Every load reads the files on the game thread, a missing asset included. */
static int asset_load(Asset *a, const char *stem, u32 id) {
    u32 bytes = 0;
    int result = asset_load_files(a, stem, id, &bytes);
    return result;
}

/* A STOP is queued behind any command the firmware has not taken yet, or held back while the queue
 * is full, and the instance's channels come back at once (firmware ABI 12). Only an upload on
 * another thread holding the host could refuse it, and every call here is on the game thread. */
static void stop_inst(afx_instance_t inst) {
    if (!inst) return;
    int result = afx_instance_stop(inst);
    if (result && result != -AFX_STALE_GENERATION)
        printf("AICAFLOW: stop of instance %lx refused (%d)\n", (unsigned long) inst, result);
}

/* Hand an asset (and its last instance) over to be freed once the instance is gone. */
static void retire(Asset *a, afx_instance_t inst) {
    if (!a->id) return;
    stop_inst(inst);
    if (a->resident) {   /* a copy of a scene-pack song: the cache keeps it */
        memset(a, 0, sizeof(*a));
        return;
    }
    for (int i = 0; i < RETIRING; i++) {
        if (!sRetiring[i].asset.id) {
            sRetiring[i].asset = *a;
            sRetiring[i].inst = inst;
            memset(a, 0, sizeof(*a));
            return;
        }
    }
    printf("AICAFLOW: no room to retire asset %lx; it stays loaded\n", (unsigned long) a->id);
    memset(a, 0, sizeof(*a));
}

static int gone(afx_instance_t inst) {
    afx_instance_status_t status;
    return !inst || afx_instance_status(inst, &status) == -AFX_STALE_GENERATION;
}

static void free_retired(void) {
    for (int i = 0; i < RETIRING; i++) {
        if (sRetiring[i].asset.id && gone(sRetiring[i].inst)) {
            /* A stop the firmware has yet to take holds the flow: its turn comes on a later pass. */
            if (afx_asset_free(sRetiring[i].asset.flow) == -AFX_ASSET_REFERENCED) continue;
            if (!sRetiring[i].asset.shared) afx_bank_release(&sRetiring[i].asset.bank);
            memset(&sRetiring[i], 0, sizeof(sRetiring[i]));
        }
    }
    level_bank_release();
}

/* Scene packs (tools/aica/sm64_scene_packs.py): the songs and sounds a level was recorded to
 * use, read in one go when the level loads instead of file by file when each first plays.
 * Read from /pc over dcload, every file is a round trip: a 0.5 KB sound took ~35 ms, so one
 * pack per scene. "SMPK", version, count, then per entry a 24-byte stem and the offsets and
 * sizes of its .afb and .afx in the file. */
#define PACK_MAGIC 0x4B504D53u   /* "SMPK" */
typedef struct {
    char stem[24];
    u32 afb_off, afb_size, afx_off, afx_size;
} PackEntry;

typedef struct {
    char stem[24];
    u32 bank, afx_off, afx_size, reserved;
} PackEntry2;

/* own is the pack's bank; the entry binds that, scene_core's, or both. */
static int pack_entry_load2(Asset *a, const u8 *base, const PackEntry2 *e, afx_bank_t *own, u32 id, u8 core) {
    afx_bank_t *bank = e->bank == PACK_BANK_CORE ? &sCoreBank : own;
    memset(a, 0, sizeof(*a));
    int result = flow_upload(a, bank, e->bank == PACK_BANK_BOTH ? &sCoreBank : NULL, base + e->afx_off,
                             e->afx_size);
    if (result) {
        printf("AICAFLOW: %.24s: flow not loaded (%d)\n", e->stem, result);
        memset(a, 0, sizeof(*a));
        return result;
    }
    a->shared = bank;
    a->holds_core = bank == &sCoreBank || e->bank == PACK_BANK_BOTH;
    a->id = id;
    a->resident = 1;
    a->core = core;
    return 0;
}

static int pack_entry_load(Asset *a, const u8 *base, const PackEntry *e, u32 id, u8 core) {
    memset(a, 0, sizeof(*a));
    int result = afx_bank_load_memory(&a->bank, base + e->afb_off, e->afb_size);
    if (result) return result;
    result = flow_upload(a, &a->bank, NULL, base + e->afx_off, e->afx_size);
    if (result) {
        afx_bank_release(&a->bank);
        memset(a, 0, sizeof(*a));
        return result;
    }
    a->id = id;
    a->resident = 1;
    a->core = core;
    return 0;
}

/* The slot a pack entry's asset goes to, with its id; NULL when it is loaded already or no
 * slot is free. */
static Asset *pack_slot(const char *stem, u32 *id) {
    Asset *table = sSfx, *slot = NULL;
    int count = SFX_ASSETS;
    if (strncmp(stem, "seq", 3) == 0) {
        *id = (u32) strtoul(stem + 3, NULL, 16) + 1;
        table = sSongs;
        count = SONG_CACHE;
    } else if (strncmp(stem, "sfx_", 4) == 0) {
        *id = (u32) strtoul(stem + 4, NULL, 16) + 1;
    } else {
        return NULL;
    }
    for (int i = 0; i < count; i++)
        if (table[i].id == *id) return NULL;
    for (int i = 0; i < count && !slot; i++)
        if (!table[i].id) slot = &table[i];
    return slot;
}

/* A version 2 pack read piece by piece: the header and entry table, then the bank, then each
 * flow, so SH4 RAM holds the largest piece instead of the whole pack (a course's is ~2 MB).
 * Returns 0 when the file is not a version 2 pack. */
static int pack_load_v2(const char *path, const char *stem, u8 core, u32 *loaded, u32 *bytes) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    u32 head[5];
    if (fread(head, 1, sizeof(head), f) != sizeof(head) || head[0] != PACK_MAGIC || (head[1] != 2 && head[1] != 3) || head[2] > 1024) {
        fclose(f);
        return 0;
    }
    PackEntry2 *entries = malloc(head[2] * sizeof(PackEntry2));
    void *piece = memalign(32, head[4]);
    afx_bank_t *own = core ? &sCoreBank : &sLevelBank;
    u8 *up = core ? &sCoreBankUp : &sLevelBankUp;
    int result = -1;
    if (entries && piece && fread(entries, sizeof(PackEntry2), head[2], f) == head[2] &&
        fseek(f, (long) head[3], SEEK_SET) == 0 && fread(piece, 1, head[4], f) == head[4]) {
        if (!core) level_bank_release();   /* the previous level's, if its flows are gone by now */
        result = *up ? -AFX_BUSY : afx_bank_load_memory(own, piece, head[4]);
    }
    free(piece);
    piece = NULL;
    *bytes = head[4];
    if (result) {
        printf("AICAFLOW: %s: bank not loaded (%d)\n", stem, result);
    } else {
        *up = 1;
        u32 capacity = 0;
        for (u32 i = 0; i < head[2]; i++) {
            const PackEntry2 *e = &entries[i];
            u32 id;
            if (e->bank == PACK_BANK_FILES) {
                prefetch(e->stem, 1);
                continue;
            }
            if (e->bank != PACK_BANK_OWN && !sCoreBankUp) continue;
            Asset *slot = pack_slot(e->stem, &id);
            if (!slot) continue;
            if (e->afx_size > capacity) {
                free(piece);
                piece = memalign(32, e->afx_size);
                capacity = piece ? e->afx_size : 0;
            }
            if (!piece || fseek(f, (long) e->afx_off, SEEK_SET) || fread(piece, 1, e->afx_size, f) != e->afx_size) continue;
            *bytes += e->afx_size;
            const PackEntry2 local = { .bank = e->bank, .afx_off = 0, .afx_size = e->afx_size };
            PackEntry2 named = local;
            memcpy(named.stem, e->stem, sizeof(named.stem));
            if (!pack_entry_load2(slot, piece, &named, own, id, core)) (*loaded)++;
        }
    }
    free(piece);
    free(entries);
    fclose(f);
    return 1;
}

static int pack_load(const char *stem, u8 core) {
    char path[128];
    /* The previous level's bank goes once the flows bound to it have: a sound still stopping at the
     * scene change holds its flow until the firmware takes the stop. Wait for that here (the screen
     * is black), or the level's pack would find the bank still up and load nothing. */
    for (int wait = 0; !core && sLevelBankUp && wait < 500; wait++) {
        free_retired();
        if (sLevelBankUp) thd_sleep(1);
    }
    if (!core && sLevelBankUp) printf("AICAFLOW: %s: the previous level's bank is still referenced\n", stem);
    void *data = NULL;
    u32 bytes = 0, loaded = 0, id;
    snprintf(path, sizeof(path), "%s/%s.smp", DC_AICAFLOW_ROOT, stem);
    int found = 1;
    /* A level's pack stays in main RAM until the scene ends (sLevelPack): a song AICA RAM lets go
     * comes back from there (pack_reload), not from the disc. scene_core is read piece by piece;
     * so is a level's pack when main RAM has no room for all of it. */
    if (!core) level_pack_drop();
    if (!core && read_file(path, &data, &bytes) == 0) {
        sLevelPack = data;
        sLevelPackBytes = bytes;
    } else if (pack_load_v2(path, stem, core, &loaded, &bytes)) {
        goto done;
    }
    int ok = (data != NULL || read_file(path, &data, &bytes) == 0) && bytes >= 20 &&
             ((const u32 *) data)[0] == PACK_MAGIC;
    const u32 *head = data;
    if (ok && head[1] == 1 && 12 + head[2] * sizeof(PackEntry) <= bytes) {
        const u8 *base = data;
        const PackEntry *e = (const PackEntry *) (base + 12);
        for (u32 n = head[2]; n--; e++) {
            if (e->afb_off + e->afb_size > bytes || e->afx_off + e->afx_size > bytes) break;
            Asset *slot = pack_slot(e->stem, &id);
            if (slot && !pack_entry_load(slot, base, e, id, core)) loaded++;
        }
    } else if (ok && (head[1] == 2 || head[1] == 3) && 20 + head[2] * sizeof(PackEntry2) <= bytes &&
               head[3] + head[4] <= bytes) {
        /* One bank for the pack: scene_core's for the run, a level's until the scene ends. */
        const u8 *base = data;
        afx_bank_t *own = core ? &sCoreBank : &sLevelBank;
        u8 *up = core ? &sCoreBankUp : &sLevelBankUp;
        if (!core) level_bank_release();   /* the previous level's, if its flows are gone by now */
        int result = *up ? -AFX_BUSY : afx_bank_load_memory(own, base + head[3], head[4]);
        if (result) {
            printf("AICAFLOW: %s: bank not loaded (%d)\n", stem, result);
        } else {
            *up = 1;
            const PackEntry2 *e = (const PackEntry2 *) (base + 20);
            for (u32 n = head[2]; n--; e++) {
                if (e->afx_off + e->afx_size > bytes) break;
                if (e->bank == PACK_BANK_FILES) {
                    prefetch(e->stem, 1);
                    continue;
                }
                if (e->bank != PACK_BANK_OWN && !sCoreBankUp) continue;
                Asset *slot = pack_slot(e->stem, &id);
                if (slot && !pack_entry_load2(slot, base, e, own, id, core)) loaded++;
            }
        }
    } else {
        ok = 0;
    }
    found = ok;
    if (data != sLevelPack) free(data);
    else if (!ok) level_pack_drop();
done:;
    int ok_all = found;   /* the pack exists (whether or not everything in it fit) */
    afx_mem_stats_t mem;
    if (afx_mem_stats(&mem) == 0) {
        printf("AICAFLOW: pack %s: %lu assets; AICA RAM used %lu KB, free %lu KB (largest %lu KB)\n", stem,
               (unsigned long) loaded, (unsigned long) (mem.used_bytes / 1024), (unsigned long) (mem.free_bytes / 1024),
               (unsigned long) (mem.largest_free_block / 1024));
    }
    return ok_all;
}

static void asset_free(Asset *a) {
    if (!a->id) return;
    if (afx_asset_free(a->flow) == 0 && (a->shared || afx_bank_release(&a->bank) == 0)) {
        memset(a, 0, sizeof(*a));
        return;
    }
    a->resident = 0;   /* still referenced: freed once its instance is gone */
    retire(a, 0);
}

/* Songs of the level's pack let go mid-scene by song_make_room, read back from the pack when
 * they play again. */
static u8 sEvictedSeq[128];

/* Sounds read from their files can be read again: those not playing let go. */
static void sfx_release_idle(void) {
    for (int i = 0; i < SFX_ASSETS; i++) {
        int playing = 0;
        for (int c = 0; c < SFX_CHANNELS; c++) playing |= sChannel[c].key + 1 == sSfx[i].id && !gone(sChannel[c].inst);
        if (sSfx[i].id && !sSfx[i].resident && !playing) asset_free(&sSfx[i]);
    }
}

/* Room in AICA RAM for a song that is not loaded (an event song the pack left out, which then
 * replaces the level's music): once `stopping` (the song it replaces) and the other retired
 * assets are gone, the pack's songs that are not playing let go of their flows; their samples
 * stay, in the scene banks. */
static void song_make_room(afx_instance_t stopping) {
    for (int p = 0; p < MUSIC_PLAYERS; p++) {   /* songs that have ended (a jingle) let go */
        Music *m = &sMusic[p];
        if (m->asset.id && gone(m->inst)) {
            retire(&m->asset, 0);
            m->inst = 0;
        }
    }
    /* A song outranks a sound read from its file, which sits in the free end of AICA RAM. */
    for (int c = 0; c < SFX_CHANNELS; c++) {
        for (int i = 0; i < SFX_ASSETS && sChannel[c].inst; i++) {
            if (sSfx[i].id == sChannel[c].key + 1 && !sSfx[i].resident) {
                retire(&sSfx[i], sChannel[c].inst);   /* stops it; freed once it is gone */
                sChannel[c].inst = 0;
            }
        }
    }
    sfx_release_idle();
    for (int wait = 0; wait < 50; wait++) {
        free_retired();
        int busy = !gone(stopping);
        for (int i = 0; i < RETIRING; i++) busy |= sRetiring[i].asset.id != 0;
        if (!busy) break;
        thd_sleep(1);
    }
    for (int i = 0; i < SONG_CACHE; i++) {
        Asset *s = &sSongs[i];
        int playing = 0;
        for (int p = 0; p < MUSIC_PLAYERS; p++) playing |= sMusic[p].inst && sMusic[p].asset.id == s->id;
        if (!s->id || s->core || playing) continue;
        for (int p = 0; p < MUSIC_PLAYERS; p++)   /* a stopped player's copy of it */
            if (sMusic[p].asset.id == s->id) memset(&sMusic[p].asset, 0, sizeof(sMusic[p].asset));
        sEvictedSeq[(s->id - 1) & 0x7f] = 1;
        asset_free(s);
    }
    free_retired();
    afx_mem_stats_t mem;
    if (afx_mem_stats(&mem) == 0)
        printf("AICAFLOW: made room: AICA RAM free %lu KB (largest %lu KB)\n", (unsigned long) (mem.free_bytes / 1024),
               (unsigned long) (mem.largest_free_block / 1024));
}

/* A song song_make_room let go: its entry alone, read from the level's pack into the cache. */
static int pack_reload(const char *stem) {
    char path[128];
    if (sLevelPack) {
        const u32 *head = (const u32 *) sLevelPack;
        const PackEntry2 *e = (const PackEntry2 *) (sLevelPack + 20);
        for (u32 i = 0; i < head[2]; i++, e++) {
            if (e->bank == PACK_BANK_FILES || strncmp(e->stem, stem, sizeof(e->stem)) != 0) continue;
            if (e->afx_off + e->afx_size > sLevelPackBytes) return -1;
            u32 id;
            Asset *slot = pack_slot(e->stem, &id);
            int up = (e->bank == PACK_BANK_OWN || sCoreBankUp) && (e->bank == PACK_BANK_CORE || sLevelBankUp);
            return slot && up ? pack_entry_load2(slot, sLevelPack, e, &sLevelBank, id, 0) : -1;
        }
        return -1;
    }
    snprintf(path, sizeof(path), "%s/scene_%02d.smp", DC_AICAFLOW_ROOT, (int) sSceneLevel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    u32 head[5];
    PackEntry2 e;
    int result = -1;
    if (fread(head, 1, sizeof(head), f) == sizeof(head) && head[0] == PACK_MAGIC && (head[1] == 2 || head[1] == 3)) {
        for (u32 i = 0; i < head[2] && fread(&e, sizeof(e), 1, f) == 1; i++) {
            if (strncmp(e.stem, stem, sizeof(e.stem)) != 0) continue;
            u32 id;
            Asset *slot = pack_slot(e.stem, &id);
            int up = (e.bank == PACK_BANK_OWN || sCoreBankUp) && (e.bank == PACK_BANK_CORE || sLevelBankUp);
            void *piece = slot && up ? memalign(32, e.afx_size) : NULL;
            if (piece && fseek(f, (long) e.afx_off, SEEK_SET) == 0 && fread(piece, 1, e.afx_size, f) == e.afx_size) {
                PackEntry2 local = e;
                local.afx_off = 0;
                result = pack_entry_load2(slot, piece, &local, &sLevelBank, id, 0);
            }
            free(piece);
            break;
        }
    }
    fclose(f);
    return result;
}

/* The credits' song does not fit beside the core pack, and the game has most sound banks disabled
 * through the credits (level_update.c): the pack goes for it, flows bound to its bank too, and
 * what still plays comes from its own files. */
static void core_drop(void) {
    for (int i = 0; i < SFX_CHANNELS; i++) {
        stop_inst(sChannel[i].inst);
        sChannel[i].inst = 0;
    }
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        retire(&sMusic[p].asset, sMusic[p].inst);
        sMusic[p].inst = 0;
    }
    for (int i = 0; i < SONG_CACHE; i++)
        if (sSongs[i].id && (sSongs[i].core || sSongs[i].holds_core)) asset_free(&sSongs[i]);
    for (int i = 0; i < SFX_ASSETS; i++)
        if (sSfx[i].id && (sSfx[i].core || sSfx[i].holds_core)) asset_free(&sSfx[i]);
    /* The bank goes once the last flow bound to it has: that waits for the stops above. */
    for (int wait = 0; wait < 50 && sCoreBankUp; wait++) {
        free_retired();
        if (afx_bank_release(&sCoreBank) == 0) sCoreBankUp = 0;
        else thd_sleep(1);
    }
    if (sCoreBankUp) printf("AICAFLOW: core bank still referenced\n");
}

/* A song that is not loaded, into a: from the pack if song_make_room let it go, else from its own
 * files; when AICA RAM runs short, song_make_room and one more try. */
static int song_fetch(Asset *a, const char *stem, u8 seq, afx_instance_t stopping) {
    int result = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt == 1) song_make_room(stopping);
        if (attempt == 2) {
            if (seq != SEQ_CREDITS) break;
            core_drop();
        }
        if (sEvictedSeq[seq] && (result = pack_reload(stem)) == 0) {
            sEvictedSeq[seq] = 0;
            for (int i = 0; i < SONG_CACHE; i++)
                if (sSongs[i].id == (u32) seq + 1) *a = sSongs[i];
            return 0;
        }
        if (!sEvictedSeq[seq] || result != -AFX_NO_AICA_RAM) result = asset_load(a, stem, (u32) seq + 1);
        if (result != -AFX_NO_AICA_RAM) break;
    }
    return result;
}

/* The previous scene ends: everything it played stops, and everything but scene_core leaves AICA
 * RAM, so the new scene starts with the room it needs. The stops are waited for (a level load
 * has time to spare) before the assets are freed. */
static void scene_end(void) {
    afx_instance_t stopped[MUSIC_PLAYERS + SFX_CHANNELS];
    int count = 0;
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        if (sMusic[p].inst) stopped[count++] = sMusic[p].inst;
        stop_inst(sMusic[p].inst);
        sMusic[p].inst = 0;
        sMusic[p].paused = 0;
    }
    for (int i = 0; i < SFX_CHANNELS; i++) {
        if (sChannel[i].inst) stopped[count++] = sChannel[i].inst;
        stop_inst(sChannel[i].inst);
        sChannel[i].inst = 0;
    }
    for (int wait = 0; wait < 200; wait++) {
        int busy = 0;
        for (int i = 0; i < count; i++) busy |= !gone(stopped[i]);
        if (!busy) break;
        thd_sleep(1);
    }
    for (int i = 0; i < SFX_CHANNELS; i++) {
        sChannel[i].fade = 1.0f;
        sChannel[i].fadeStep = 0.0f;
    }
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        if (sMusic[p].asset.id && !sMusic[p].asset.resident) asset_free(&sMusic[p].asset);
        memset(&sMusic[p].asset, 0, sizeof(sMusic[p].asset));
    }
    for (int i = 0; i < SONG_CACHE; i++) {
        if (sSongs[i].id && !sSongs[i].core) asset_free(&sSongs[i]);
    }
    for (int i = 0; i < SFX_ASSETS; i++) {
        if (sSfx[i].id && !sSfx[i].core) asset_free(&sSfx[i]);
    }
    free_retired();
    if (sLevelBankUp) sLevelBankDying = 1;
    level_bank_release();
    level_pack_drop();
    for (int i = 0; i < PREFETCH_MAX; i++)
        if (sPrefetch[i].keep) prefetch_drop(i);
    /* A song or sound that failed for want of AICA RAM gets another chance. */
    memset(sMissingSeq, 0, sizeof(sMissingSeq));
    memset(sEvictedSeq, 0, sizeof(sEvictedSeq));
    sMissingSfxCount = 0;
}

/* Called from init_level, and from the first song or sound of a level that does not run it (the
 * title). The level's pack is read each time the level starts. */
/* The title and file select are the menu scene (level 1), also when the title comes back after a
 * demo and gCurrLevelNum is still the demo's level: sMenuOver is that level, until a level starts
 * (init_level) or gCurrLevelNum changes. */
static s16 sMenuOver = -1;

static s16 scene_level(void) {
    if (gCurrLevelNum != sMenuOver) sMenuOver = -1;
    return sMenuOver >= 0 ? 1 : gCurrLevelNum;
}

void sm64_afx_menu_enter(void) {
    if (gCurrLevelNum != 1) sMenuOver = gCurrLevelNum;
}

void sm64_afx_scene_prepare(s16 level) {
    if (level != 1) sMenuOver = -1;
    if (!sReady || level == sSceneLevel) return;
    /* The credits' song plays on through their levels, whose packs stay out. */
    if (sMusic[0].seq == SEQ_CREDITS && sm64_afx_music_playing(0)) {
        sSceneLevel = level;
        return;
    }
    if (sSceneLevel >= 0) scene_end();
    s16 from = sSceneLevel;
    sSceneLevel = level;
    if (!sCorePackTried) {
        sCorePackTried = 1;
        pack_load("scene_core", 1);
    }
    if (level < 0 || level >= (s16) sizeof(sPackAbsent) || sPackAbsent[level]) return;
    char stem[16];
    snprintf(stem, sizeof(stem), "scene_%02d", (int) level);
    if (!pack_load(stem, 0)) sPackAbsent[level] = 1;
    /* Bowser 3 goes on to the castle grounds only for the ending (sCreditsSequence[0]): Peach's
     * cutscene, then the credits' song, which is in no pack (620 KB, it takes the core pack's room),
     * and the sounds the credits play after that from their own files (seen on hardware
     * 2026-11-07: the waterfall, two bird chirps, the birds flying away, the closing voice). Read
     * them into main RAM now, in this load, so that the credits make no disc read. */
    if (from == LEVEL_BOWSER_3_NUM && level == LEVEL_CASTLE_GROUNDS_NUM) {
        static const char *const credits[] = { "seq1A", "sfx_40010000", "sfx_80500000", "sfx_90520000",
                                               "sfx_30690000", "sfx_701F0000" };
        for (u32 i = 0; i < sizeof(credits) / sizeof(credits[0]); i++) prefetch(credits[i], 0);
    }
}

int sm64_afx_init(void) {
    int result = afx_init(gAicaflowFirmware, gAicaflowFirmwareSize);
    printf("AICAFLOW: init %d (firmware %u bytes, assets from %s)\n", result, gAicaflowFirmwareSize,
           DC_AICAFLOW_ROOT);
    sReady = result == 0;
    for (int i = 0; i < SFX_CHANNELS; i++) sChannel[i].fade = 1.0f;
    /* The ring goes in before any asset, at the arena's start; sound_reset replaces the program
     * in the same place when a preset changes it. */
    sm64_sound_reverb_preset(0);
    return result;
}

/* The N64 send (0..127, reverb / 128 of the voice) as AICA IMXL, which steps 3 dB from -42 dB (1)
 * to 0 dB (15): the least send level of each code. tools/aica/sm64_afx.py maps songs' sends the
 * same way. */
static const u8 sSendLevel[15] = { 1, 2, 2, 3, 4, 5, 7, 10, 14, 20, 28, 39, 54, 77, 108 };
static u8 send_code(u8 reverb) {
    u8 code = 0;
    while (code < 15 && reverb >= sSendLevel[code]) code++;
    return code;
}

/* SM64's reverb (synthesis.c, US): each update starts its output with the ring's oldest samples
 * at unity and writes back those times reverbGain / 32768 plus the notes' sends. The delay
 * prefab is that at 44.1 kHz; its input coefficient (6) raised to unity as AICAflow's own
 * calibrated echo (afx_dsp_program_demo) does, where the prefab keeps a quarter for headroom.
 * The longest preset (0x1000 at 32 kHz, 5645 samples) fits the smallest ring (RBL 0, 8 Ki). */
void sm64_afx_reverb(u16 windowSize, u16 gain) {
    static u16 sReverbSamples;
    static s16 sReverbFeedback;
    if (!sReady) return;
    u16 samples = (u16) (((u32) windowSize * 44100u + 16000u) / 32000u);
    s16 feedback = (s16) ((gain > 24576 ? 24576 : gain) & ~7);
    if (samples == sReverbSamples && feedback == sReverbFeedback) return;
    afx_dsp_program_t program;
    int result = afx_dsp_program_delay(&program, samples, feedback, false);
    if (!result) result = afx_dsp_program_coefficient(&program, 6, 32760);
    if (!result) result = afx_dsp_scene_program_ring(&program, sizeof(program), 0);
    if (!result && !gDcReverbOn) afx_dsp_scene_returns(false);   /* a new program returns at full */
    printf("AICAFLOW: reverb %u samples, feedback %d: %d\n", samples, feedback, result);
    if (result) samples = 0;   /* DSP is off: try again at the next preset */
    sReverbSamples = samples;
    sReverbFeedback = feedback;
}

int gDcReverbOn = 1;

void sm64_afx_reverb_returns(void) {
    if (sReady) afx_dsp_scene_returns(gDcReverbOn != 0);   /* no program yet: the next one applies it */
}

static void music_apply_gain(Music *m) {
    /* The castle's and the endless stairs' songs soften under the pause menu (their seq_setmutebhv
     * 0x20, seq_setmutescale 40); the others stop until it closes. */
    f32 g = sMuted && (m->seq == 0x04 || m->seq == 0x18) ? m->gain * 40.0f / 127.0f : m->gain;
    u8 v = g <= 0.0f ? 0 : g >= 1.0f ? 255 : (u8) (g * 255.0f + 0.5f);
    if (m->inst && v != m->sentGain) {
        afx_instance_gain(m->inst, v);
        m->sentGain = v;
    }
}

/* Under the pause menu: AFX_CMD_PAUSE releases the song's voices and holds its place, and a rebuild
 * with no channel states starts it there again. Retried each tick while a command is in flight. */
static void music_mute_step(Music *m) {
    if (!m->inst || gone(m->inst)) {
        m->paused = 0;
    } else if (sMuted && !m->paused && m->seq != 0x04 && m->seq != 0x18) {
        if (afx_instance_pause(m->inst) == 0) {
            m->paused = 1;
            m->pausedAt = afx_status_timer_ticks();
        }
    } else if (!sMuted && m->paused) {
        afx_instance_status_t st;
        if (afx_instance_status(m->inst, &st) == 0 && st.state == AFX_PAUSED) {
            s32 wait = (s32) (st.next_deadline - m->pausedAt);
            if (afx_instance_rebuild(m->inst, NULL, 0, st.position, 0,
                                     afx_status_timer_ticks() + (wait > 0 ? (u32) wait : 1u), true) == 0) {
                m->paused = 0;
            }
        }
    }
}

int sm64_afx_play_sequence(u8 player, u8 seqId) {
    if (!sReady || player >= MUSIC_PLAYERS) return 0;
    u8 seq = seqId & 0x7f;
    if (seq == 0) {   /* SEQ_SOUND_PLAYER in a music player: a level without music */
        sm64_afx_music_stop(player);
        return 0;
    }
    if ((seq == 0x02 || seq == 0x21) && gCurrLevelNum != 1) sMenuOver = gCurrLevelNum;   /* title, file select */
    sm64_afx_scene_prepare(scene_level());
    Music *m = &sMusic[player];
    DBG("AFXDBG t=%lu play player=%u seq=%02X loaded=%lx\n", (unsigned long) sTicks, player, seqId,
        (unsigned long) m->asset.id);
    afx_instance_t old = m->inst, stopping = 0;
    m->inst = 0;
    m->paused = 0;
    if (m->asset.id != (u32) seq + 1) {
        retire(&m->asset, old);   /* stops it; freed once it is gone */
        stopping = old;
        old = 0;
    }
    if (old) stop_inst(old);
    for (int i = 0; i < SONG_CACHE && m->asset.id != (u32) seq + 1; i++)
        if (sSongs[i].id == (u32) seq + 1) m->asset = sSongs[i];
    if (m->asset.id != (u32) seq + 1) {
        if (sMissingSeq[seq]) return 0;
        char stem[16];
        snprintf(stem, sizeof(stem), "seq%02X", seq);
        if (song_fetch(&m->asset, stem, seq, stopping)) {
            sMissingSeq[seq] = 1;
            printf("AICAFLOW: no asset for sequence %02X; silent\n", seq);
            return 0;
        }
    }
    int result = afx_instance_activate(m->asset.flow, &m->inst);
    if (result) {
        printf("AICAFLOW: sequence %02X not started (%d)\n", seq, result);
        m->inst = 0;
        return 0;
    }
    m->seq = seq;
    m->sentGain = 255;
    memset(m->sentLane, 255, sizeof(m->sentLane));
    return 1;
}

int sm64_afx_music_playing(u8 player) {
    return sMusic[player].inst && !gone(sMusic[player].inst);
}

void sm64_afx_music_gain(u8 player, f32 gain) {
    sMusic[player].gain = gain;
    music_apply_gain(&sMusic[player]);
}

/* The songs whose channels fade or start apart: lane n + 1 of the flow holds the sequence channels
 * of mask n, lane 0 the rest (tools/aica/sm64_afx_batch.py has the same table). */
static const struct { u8 seq; u16 masks[3]; } sSongLanes[] = {
    { 0x05, { 0x0003, 0x0e40, 0xc000 } },
    { 0x0A, { 0x0070, 0, 0 } },
    { 0x0C, { 0x0100, 0x0008, 0 } },
};

void sm64_afx_music_channels(u8 player, u16 enabled, const f32 *scale) {
    Music *m = &sMusic[player];
    u8 values[32] = { 0 };
    u32 mask = 0;
    if (!m->inst || !m->asset.lanes) return;
    for (u32 s = 0; s < sizeof(sSongLanes) / sizeof(sSongLanes[0]); s++) {
        if (sSongLanes[s].seq != m->seq) continue;
        for (int lane = 0; lane < 4 && lane < m->asset.lanes; lane++) {
            u16 channels = lane ? sSongLanes[s].masks[lane - 1] : 0;
            if (lane == 0) {
                channels = 0xffff;
                for (int n = 0; n < 3; n++) channels &= (u16) ~sSongLanes[s].masks[n];
            }
            int first = 0;
            while (first < 16 && !(channels >> first & 1)) first++;
            f32 g = first < 16 && enabled >> first & 1 ? scale[first] : 0.0f;
            values[lane] = g <= 0.0f ? 0 : g >= 1.0f ? 255 : (u8) (g * 255.0f + 0.5f);
            if (values[lane] != m->sentLane[lane]) mask |= 1u << lane;
        }
    }
    /* A full command queue leaves them to the next tick. */
    if (mask && afx_instance_lanes_set(m->inst, AFX_LANE_GAIN, 0, mask, values) == 0) {
        memcpy(m->sentLane, values, sizeof(m->sentLane));
    }
}

void sm64_afx_music_stop(u8 player) {
    Music *m = &sMusic[player];
    if (m->inst) {
        DBG("AFXDBG t=%lu stop player=%u\n", (unsigned long) sTicks, player);
        stop_inst(m->inst);
        m->inst = 0;
        m->paused = 0;
    }
}

void sm64_afx_sfx_start(u8 channel, u32 soundBits) {
    if (!sReady) return;
    sm64_afx_scene_prepare(scene_level());
    SfxChannel *c = &sChannel[channel];
    if (c->inst && !gone(c->inst)) stop_inst(c->inst);   /* the channel's sound makes way */
    c->inst = 0;
    u32 key = soundBits & 0xF0FF0000u;
    Asset *a = NULL;
    for (int i = 0; i < SFX_ASSETS && !a; i++)
        if (sSfx[i].id == key + 1) a = &sSfx[i];
    if (a == NULL) {
        for (u32 i = 0; i < sMissingSfxCount; i++)
            if (sMissingSfx[i] == key) return;
        for (int i = 0; i < SFX_ASSETS && !a; i++)
            if (!sSfx[i].id) a = &sSfx[i];
        if (a == NULL) return;
        char stem[24];
        snprintf(stem, sizeof(stem), "sfx_%08lX", (unsigned long) key);
        int result = asset_load(a, stem, key + 1);
        if (result == -AFX_NO_AICA_RAM) {
            sfx_release_idle();
            result = asset_load(a, stem, key + 1);
        }
        if (result) {
            /* One that found no room may next time. */
            if (result != -AFX_NO_AICA_RAM && sMissingSfxCount < sizeof(sMissingSfx) / sizeof(sMissingSfx[0])) {
                sMissingSfx[sMissingSfxCount++] = key;
            }
            printf("AICAFLOW: no asset for sound %08lX; silent (%d)\n", (unsigned long) soundBits, result);
            return;
        }
    }
    if (afx_instance_activate(a->flow, &c->inst)) {
        c->inst = 0;
        return;
    }
    c->key = key;
    c->gain = 255;
    c->pan = 0;
    c->pitch = 0;
    c->send = 255;
    c->script = 0;
    for (u32 i = 0; i < sizeof(sSfxScriptReverb) / sizeof(sSfxScriptReverb[0]); i++)
        if (sSfxScriptReverb[i].key == key) c->script = sSfxScriptReverb[i].reverb;
}

int sm64_afx_sfx_active(u8 channel) {
    return sChannel[channel].inst && !gone(sChannel[channel].inst);
}

void sm64_afx_sfx_stop(u8 channel) {
    stop_inst(sChannel[channel].inst);
    sChannel[channel].inst = 0;
}

/* The volume, pan and pitch update_game_sound gives the channel, sent when they change: the
 * instance's gain, and the pan and pitch of lane 0 (sounds are built with a lane map holding every
 * channel in lane 0; AICA pan steps are about 3 dB, 15 per side; a lane's pitch multiplies by
 * 1 + value / 256). The reverb send follows the volume: the
 * lane scales the flows' full send (IMXL 15) to code c with c * 17 (the firmware's rounded
 * product / 255). The menu bank has the script's send alone, and plays on under the pause menu. */
void sm64_afx_sfx_place(u8 channel, u32 soundBits, f32 volume, f32 pan, f32 pitch, int echo) {
    SfxChannel *c = &sChannel[channel];
    if (!c->inst) return;
    f32 heard = sMuted && channel != MENU_BANK ? 0.0f : volume * c->fade;
    u8 gain = heard <= 0.0f ? 0 : heard >= 1.0f ? 255 : (u8) (heard * 255.0f + 0.5f);
    s8 offset = (s8) (pan <= 0.0f ? -15 : pan >= 1.0f ? 15 : (s32) ((pan - 0.5f) * 30.0f + (pan >= 0.5f ? 0.5f : -0.5f)));
    if (gain != c->gain) {
        afx_instance_gain(c->inst, gain);
        c->gain = gain;
    }
    if (offset != c->pan) {
        u8 values[32] = { (u8) offset };
        afx_instance_lanes_set(c->inst, AFX_LANE_PAN, 0, 1u, values);
        c->pan = offset;
    }
    s32 bend = (s32) ((pitch - 1.0f) * 256.0f + (pitch >= 1.0f ? 0.5f : -0.5f));
    if (bend < -128) bend = -128;
    if (bend > 127) bend = 127;
    if (bend != c->pitch) {
        u8 values[32] = { (u8) bend };
        afx_instance_lanes_set(c->inst, AFX_LANE_PITCH, 0, 1u, values);
        c->pitch = (s8) bend;
    }
    u8 send = (u8) (send_code(echo ? sm64_sound_reverb(soundBits, volume, c->script) : c->script) * 17);
    if (send != c->send) {
        u8 values[32] = { send };
        afx_instance_lanes_set(c->inst, AFX_LANE_DSP_SEND, 0, 1u, values);
        c->send = send;
    }
}

void sm64_afx_sfx_fade(u8 channel, u16 ticks) {
    sChannel[channel].fadeStep = ticks ? sChannel[channel].fade / (f32) ticks : sChannel[channel].fade;
}

void sm64_afx_mute(u8 muted) {
    sMuted = muted;
}

/* sound_reset: as disable_all_sequence_players does to the sequencer. */
void sm64_afx_stop_all(void) {
    for (int p = 0; p < MUSIC_PLAYERS; p++) sm64_afx_music_stop((u8) p);
    for (int i = 0; i < SFX_CHANNELS; i++) {
        stop_inst(sChannel[i].inst);
        sChannel[i].inst = 0;
        sChannel[i].fade = 1.0f;
        sChannel[i].fadeStep = 0.0f;
    }
    sMuted = 0;
}

/* AICA's MONO bit, in the register that holds the master volume too: that does not read back, so
 * this writes the full volume the firmware sets at start. */
static u32 sMonoBit;
static u8 sSilent;

void sm64_afx_mono(u8 mono) {
    sMonoBit = mono ? 0x8000u : 0;
    g2_write_32(0xa0702800, sMonoBit | (sSilent ? 0u : 0x000fu));
}

/* Show star's way back (tas_slice.c) replays the player's moves fast: the master volume is 0 then. */
void sm64_afx_silence(u8 silent) {
    sSilent = silent;
    g2_write_32(0xa0702800, sMonoBit | (silent ? 0u : 0x000fu));
}

void sm64_afx_tick(void) {
    if (!sReady) return;
    for (int i = 0; i < SFX_CHANNELS; i++) {
        SfxChannel *c = &sChannel[i];
        if (c->fadeStep > 0.0f) {
            c->fade = c->fade > c->fadeStep ? c->fade - c->fadeStep : 0.0f;
        }
    }
    for (int p = 0; p < MUSIC_PLAYERS; p++) music_mute_step(&sMusic[p]);
    free_retired();
}

