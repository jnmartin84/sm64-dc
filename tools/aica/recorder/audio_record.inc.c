// Included by the recorder's main.c, which gives it rec_wait_tick, rec_now_us, rec_dir, rec_seconds,
// rec_sequences and rec_sounds. Plays each sequence and each sound (comma-separated, hex) in turn
// while the AICA driver's command log drains to rec_seqXX.bin or rec_sfx_XXXXXXXX.bin. The game
// loop's 30 Hz tick is what lets the audio code process requests, so it is signalled here every
// other vblank. An item ends by itself: a sequence one second after its second jump back (two
// passes, what tools/aica/sm64_afx.py needs to loop it), or when nothing has sounded for two
// seconds; a sound when nothing has sounded for one second; or at rec_seconds.
#include "audio/aica_record.h"
#include "audio/data.h"
#include "audio/external.h"
#include "audio/load.h"
#include "seq_ids.h"
typedef struct {
    int voices, started, jumps;
    uint64_t quiet_since, second_jump;
    u32 targets[16];   /* jump targets seen: the loop is the second jump to one of them */
} RecWatch;
static u32 rec_drain(FILE *f, RecWatch *w, uint64_t now) {
    u32 n = 0;
    while (gAicaRecTail != gAicaRecHead) {
        const AicaRecord *r = aica_record_at(gAicaRecTail);
        if (f != NULL) fwrite(r, sizeof(AicaRecord), 1, f);
        if (r->cmd == AICA_REC_START) { w->voices++; w->started = 1; }
        if (r->cmd == AICA_REC_STOP && w->voices > 0) w->voices--;
        if (r->cmd == AICA_REC_SEQ_JUMP && r->ch == SEQ_PLAYER_LEVEL) {
            int seen = 0;
            for (int i = 0; i < w->jumps && i < 16; i++) seen |= w->targets[i] == r->key;
            if (w->jumps < 16) w->targets[w->jumps] = r->key;
            w->jumps++;
            if (seen && !w->second_jump) w->second_jump = now;
        }
        gAicaRecTail++;
        n++;
    }
    if (w->voices) w->quiet_since = 0;
    else if (!w->quiet_since) w->quiet_since = now;
    return n;
}
static void rec_tick(void) {
    rec_wait_tick();
    audio_signal_game_loop_tick();
}
static void rec_item(int seq, u32 sound) {
    static f32 sPos[3];
    static int sSfxPlayer;
    char path[512];
    if (seq >= 0) snprintf(path, sizeof(path), "%srec_seq%02X.bin", rec_dir(), seq);
    else snprintf(path, sizeof(path), "%srec_sfx_%08lX.bin", rec_dir(), (unsigned long) sound);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("AUDIOREC cannot write %s\n", path);
        return;
    }
    AicaRecordHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = AICA_RECORD_MAGIC;
    h.updates_per_frame = (u32) gAudioUpdatesPerFrame;
    h.wave_count = 4;
    h.band_count = 1;
    h.wave_samples = 64;
    h.record_bytes = sizeof(AicaRecord);
    fwrite(&h, sizeof(h), 1, f);
    for (int w = 0; w < 4; w++) {
        fwrite(gWaveSamples[w], sizeof(s16), 64, f);
    }
    if (seq < 0 && !sSfxPlayer) {
        // Sounds play through the SFX sequence player, which the game starts in its own init.
        play_music(SEQ_PLAYER_SFX, SEQUENCE_ARGS(0, SEQ_SOUND_PLAYER), 0);
        for (int i = 0; i < 15; i++) rec_tick();
        sSfxPlayer = 1;
    }
    RecWatch w;
    memset(&w, 0, sizeof(w));
    rec_drain(NULL, &w, 0);   // whatever came before this item does not belong to it
    memset(&w, 0, sizeof(w));
    u32 sub0 = gAicaRecTick;
    uint64_t us0 = rec_now_us(), start = rec_now_us() / 1000;
    if (seq >= 0) play_music(SEQ_PLAYER_LEVEL, SEQUENCE_ARGS(4, seq), 0);
    else play_sound((s32) sound, sPos);
    // A sound without the 0x80 flag is continuous: the game keeps it alive by requesting it every
    // frame and stops it when the requests stop. Hold it for four seconds, then let it go.
    int hold = (seq < 0 && !(sound & 0x80)) ? 120 : 0;
    u32 records = 0;
    for (;;) {
        rec_tick();
        if (hold && --hold) play_sound((s32) sound, sPos);
        uint64_t now = rec_now_us() / 1000;
        records += rec_drain(f, &w, now);
        if (now - start >= (uint64_t) rec_seconds(seq) * 1000) break;
        if (w.second_jump && now - w.second_jump >= 1000) break;
        if (w.started && w.quiet_since && now - w.quiet_since >= (seq >= 0 ? 2000u : 1000u)) break;
        if (!w.started && now - start >= 5000) break;
    }
    u32 subs = gAicaRecTick - sub0;
    u32 us = (u32) (rec_now_us() - us0);
    AicaRecord end = { gAicaRecTick, AICA_REC_END, 0, 0, 0, us, subs, 0, 0, { 0 } };
    fwrite(&end, sizeof(end), 1, f);
    fclose(f);
    printf("AUDIOREC %s: %u records, %d jumps, %u sub-updates in %u us\n", path, (unsigned) records, w.jumps,
           (unsigned) subs, (unsigned) us);
    // Silence before the next item: stop the music and let every voice end.
    if (seq >= 0) stop_background_music(SEQUENCE_ARGS(4, seq));
    uint64_t t0 = rec_now_us() / 1000;
    do {
        rec_tick();
        rec_drain(NULL, &w, rec_now_us() / 1000);
    } while ((w.voices || rec_now_us() / 1000 - t0 < 1000) && rec_now_us() / 1000 - t0 < 8000);
}
static void audio_record_session(void) {
    printf("AUDIOREC updates_per_frame=%d seqs=%s sounds=%s\n", (int) gAudioUpdatesPerFrame, rec_sequences(),
           rec_sounds());
    for (const char *p = rec_sequences(); *p;) {
        char *e;
        unsigned long v = strtoul(p, &e, 16);
        if (e == p) break;
        rec_item((int) v, 0);
        p = *e == ',' ? e + 1 : e;
    }
    for (const char *p = rec_sounds(); *p;) {
        char *e;
        unsigned long v = strtoul(p, &e, 16);
        if (e == p) break;
        rec_item(-1, (u32) v);
        p = *e == ',' ? e + 1 : e;
    }
    printf("AUDIOREC done, %u dropped\n", (unsigned) gAicaRecDropped);
}
