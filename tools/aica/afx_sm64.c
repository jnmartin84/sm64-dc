/* Build AICAflow assets from a job written by tools/aica/sm64_afx.py.
 *
 *   clang -std=c11 -O2 -Wall -Wextra -Werror -I$AICAFORGE/dependencies/AICAflow/driver/format/include \
 *       -I$AICAFORGE/src tools/aica/afx_sm64.c $AICAFORGE/src/afx_compile_c.c \
 *       $AICAFORGE/dependencies/AICAflow/driver/format/src/codec.c -lm -o build/afx_sm64
 *   build/afx_sm64 song.job song.afb song.afx     (writes song.afc too)
 *   AFX_SIMPLIFY=2,1 build/afx_sm64 ...           (lossy: see afx_c_simplify_events)
 *
 * The job: "SJOB", zone count, event count, duration ticks, loop tick (~0 for
 * none), tick rate numerator, channels, tick rate denominator (0 = 1); then each zone (format, loop, loop start, loop
 * end, frames, bytes, data padded to 4) and each event (tick, order, opcode,
 * channel, setup, mask, 18 field words). Zones are complete AICA samples, and a
 * NOTE event selects one by index and carries the words it sets. The flow's
 * setups come from AICAforge's setup dictionary (afx_c_optimize_events): one per
 * envelope, pan and send combination the notes use often enough, so most NOTEs
 * carry only their pitch and level. Every NOTE writes the same words as before. */
#include <aicaflow/codec.h>
#include "afx_compile_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, uint32_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size > 0 ? (size_t)size : 1);
    if (!data || (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size)) { fclose(f); free(data); return NULL; }
    fclose(f);
    *bytes = (uint32_t)size;
    return data;
}
static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fwrite(data, 1, bytes, f) != bytes;
    return fclose(f) || bad ? -1 : 0;
}
static int sibling(const char *path, const char *suffix, char *out, size_t cap) {
    const char *dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    if (stem + strlen(suffix) + 1 > cap) return -1;
    memcpy(out, path, stem); strcpy(out + stem, suffix);
    return 0;
}

/* Insert a lane map between the setups and the stream and set AFX_FLAG_LANES, for the host's
 * afx_instance_lanes_set: every channel in lane 0, so it can pan a whole sound, or map, a file of
 * one lane per channel, so it can fade a song's sequence channels.
 * The stream moves by the map's size: LOOP targets are stream-relative and setups keep their
 * place, so only the header changes. AFC checkpoints hold image positions: dropped here. */
static int add_lanes(afx_c_output_t *out, const char *map) {
    afx_file_header_t h;
    if (afx_file_validate(out->afx, out->afx_bytes, &h)) return -1;
    uint32_t lanes = h.required_channels, setups = h.setup_count * AFX_SETUP_BYTES;
    uint32_t bytes = out->afx_bytes + lanes;
    uint8_t *afx = calloc(1, bytes);
    if (!afx) return -1;
    memcpy(afx, out->afx, h.image_offset + setups);
    if (map) {
        FILE *f = fopen(map, "rb");
        size_t got = f ? fread(afx + h.image_offset + setups, 1, lanes, f) : 0;
        if (f) fclose(f);
        if (got != lanes) return free(afx), -1;
    }
    memcpy(afx + h.image_offset + setups + lanes, out->afx + h.image_offset + h.stream_offset, h.stream_size);
    h.flags |= AFX_FLAG_LANES;
    h.stream_offset = setups + lanes;
    h.image_size += lanes;
    h.total_size = bytes;
    h.control_id = afx_control_id(afx + h.image_offset, h.image_size);
    afx_encode_header(afx, &h);
    free(out->afx); out->afx = afx; out->afx_bytes = bytes;
    free(out->afc); out->afc = NULL; out->afc_bytes = 0;
    return afx_c_set_work_profile(out->afx, out->afx_bytes);
}

int main(int argc, char **argv) {
    int lanes = argc >= 5 && !strcmp(argv[4], "--lanes");
    const char *lane_map = lanes && argc == 6 ? argv[5] : NULL;
    if (lanes) argc = 4;
    if (argc != 4) return fprintf(stderr, "usage: %s song.job out.afb out.afx [--lanes [map]]\n", argv[0]), 2;
    uint32_t bytes;
    uint8_t *job = read_file(argv[1], &bytes);
    if (!job || bytes < 32 || afx_read32(job) != 0x424F4A53u) return fprintf(stderr, "bad job %s\n", argv[1]), 1;
    uint32_t zone_count = afx_read32(job + 4), event_count = afx_read32(job + 8), duration = afx_read32(job + 12);
    uint32_t loop_tick = afx_read32(job + 16), rate_num = afx_read32(job + 20), channels = afx_read32(job + 24);
    uint32_t rate_den = afx_read32(job + 28) ? afx_read32(job + 28) : 1u;
    /* The compiler takes whole ticks per second; the exact rational rate goes in the header after. */
    uint32_t rate = (rate_num + rate_den / 2u) / rate_den;
    afx_c_zone_t *zones = calloc(zone_count, sizeof(*zones));
    afx_c_event_t *events = calloc(event_count, sizeof(*events));
    if (!zones || !events) return 1;
    uint32_t at = 32;
    for (uint32_t i = 0; i < zone_count; ++i) {
        if (at + 24 > bytes) return fprintf(stderr, "truncated job\n"), 1;
        uint32_t fmt = afx_read32(job + at), loop = afx_read32(job + at + 4), loop_start = afx_read32(job + at + 8);
        uint32_t loop_end = afx_read32(job + at + 12), frames = afx_read32(job + at + 16), size = afx_read32(job + at + 20);
        at += 24;
        if (at + size > bytes) return fprintf(stderr, "truncated job\n"), 1;
        zones[i] = (afx_c_zone_t){
            .sample = { job + at, size, frames, (uint8_t)fmt, 60, (uint8_t)loop,
                        (uint16_t)loop_start, (uint16_t)loop_end, 0, 44100 },
            .key_max = 127, .velocity_max = 127 };
        at += (size + 3u) & ~3u;
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        if (at + 52 > bytes) return fprintf(stderr, "truncated job\n"), 1;
        afx_c_event_t *e = &events[i];
        e->tick = afx_read32(job + at); e->order = afx_read32(job + at + 4);
        e->opcode = job[at + 8]; e->channel = job[at + 9];
        e->setup = afx_read16(job + at + 10); e->mask = afx_read32(job + at + 12);
        for (uint32_t f = 0; f < 18; ++f) e->fields[f] = afx_read16(job + at + 16 + 2 * f);
        at += 52;
    }
    (void)channels;
    /* AFX_SIMPLIFY="pan,level": AICAforge's lossy trace simplification (afx_c_simplify_events),
     * pan steps and 0.375 dB level steps a dropped change may differ from the last one kept. */
    const char *simplify = getenv("AFX_SIMPLIFY");
    if (simplify) {
        unsigned pan = 0, level = 0;
        if (sscanf(simplify, "%u,%u", &pan, &level) != 2 || pan > 15 || level > 255)
            return fprintf(stderr, "AFX_SIMPLIFY must be pan,level (got %s)\n", simplify), 2;
        afx_c_simplify_t options = {.pan_tolerance = (uint8_t)pan, .level_tolerance = (uint8_t)level,
                                    .loop_tick = loop_tick == UINT32_MAX ? AFX_C_NO_LOOP : loop_tick};
        if (afx_c_simplify_events(events, &event_count, &options))
            return fprintf(stderr, "cannot simplify %s\n", argv[1]), 1;
    }
    /* Needs AICAforge with the template count fix (fix/optimize-templates): before it, more
     * setups than zones overflowed. */
    afx_c_event_t *notes = NULL;
    afx_c_zone_t *setups = NULL;
    uint32_t setup_count = 0;
    if (afx_c_optimize_events(events, event_count, zones, zone_count, &notes, &setups, &setup_count))
        return fprintf(stderr, "cannot build the setups of %s\n", argv[1]), 1;
    afx_c_output_t out;
    int result = loop_tick == UINT32_MAX
        ? afx_c_compile_events(notes, event_count, duration, rate, setups, setup_count, &out)
        : afx_c_compile_events_loop(notes, event_count, duration, loop_tick, rate, setups, setup_count, &out);
    if (result) return fprintf(stderr, "cannot compile %s (%u setups, %u events, %u ticks)\n", argv[1], setup_count, event_count, duration), 1;
    /* Header words 68/72: the authored tick rate. Not part of the control image, so the
     * control identity (and with it the AFC binding) is unchanged. */
    afx_write32(out.afx + 68, rate_num);
    afx_write32(out.afx + 72, rate_den);
    if (afx_file_validate(out.afx, out.afx_bytes, NULL)) return fprintf(stderr, "invalid AFX after the rate patch\n"), 1;
    if (lanes && add_lanes(&out, lane_map)) return fprintf(stderr, "cannot add the lane map\n"), 1;
    char afc[4096];
    if (write_file(argv[2], out.afb, out.afb_bytes) || write_file(argv[3], out.afx, out.afx_bytes) ||
        (out.afc && (sibling(argv[3], ".afc", afc, sizeof(afc)) || write_file(afc, out.afc, out.afc_bytes))))
        return fprintf(stderr, "cannot write outputs\n"), 1;
    printf("wrote %s (%u bytes) and %s (%u bytes, %u stream bytes%s)\n", argv[2], out.afb_bytes, argv[3], out.afx_bytes,
           afx_read32(out.afx + 28), loop_tick == UINT32_MAX ? "" : ", looping");
    afx_c_output_free(&out);
    free(setups); free(notes); free(zones); free(events); free(job);
    return 0;
}
