#ifndef AICA_RECORD_H
#define AICA_RECORD_H
/* DC_AUDIO_RECORD: the AICA voice driver's command log (src/audio/aica_synth.c),
   drained to /pc/sm64_audio_rec.bin by src/pc/pc_main.c and read by
   tools/aica/sm64_afx.py. Little-endian, 20-byte records after a header (16-byte ones, without
   reverb, in logs whose header has no record size). */
#include <PR/ultratypes.h>

#define AICA_RECORD_MAGIC 0x43455241u   /* "AREC" */
#define AICA_RECORD_RING 8192

enum {
    AICA_REC_START = 1,    /* ch, vol, pan, freq (Hz), key: sample table key, or synth */
    AICA_REC_UPDATE = 2,   /* ch, vol, pan, freq */
    AICA_REC_STOP = 3,     /* ch */
    AICA_REC_SEQ_JUMP = 4, /* ch = sequence player, key = target offset in the sequence */
    AICA_REC_END = 5,      /* the recording ends here: freq = microseconds since the first sub-update,
                              key = sub-updates run, so the measured rate is key * 1e6 / freq */
};
/* START keys: a sample-table key (offset into sound_data.tbl), or a synthetic
   wave: bit 31 set, waveform in the low bits, band (0..3) in bits 24..27. A
   long sample the SH4 streams has bit 30 set. */
#define AICA_REC_KEY_SYNTH 0x80000000u
#define AICA_REC_KEY_STREAM 0x40000000u

typedef struct {
    u32 tick;   /* sequencer sub-update count (gAudioUpdatesPerFrame per 60 Hz frame) */
    u8 cmd, ch, vol, pan;
    u32 freq;
    u32 key;
    u8 reverb;  /* START/UPDATE: the note's reverb send (0..127), as the N64 mixer would use it */
    u8 seqChannel;   /* START: the note's channel in its sequence, which music dynamics fade */
    u8 pad[2];
} AicaRecord;

typedef struct {
    u32 magic;
    u32 updates_per_frame;
    u32 wave_count, band_count, wave_samples;   /* the synthetic wave buffers follow: s16[wave][band][samples] */
    u32 record_bytes;   /* sizeof(AicaRecord); 0 in older logs (16-byte records) */
    u32 reserved[2];
} AicaRecordHeader;

extern volatile u32 gAicaRecHead, gAicaRecTail;
extern u32 gAicaRecTick, gAicaRecDropped;
extern u64 gAicaRecStartUs;
void aica_record(u8 cmd, u8 ch, u8 vol, u8 pan, u32 freq, u32 key, u8 reverb);
const AicaRecord *aica_record_at(u32 index);
#endif
