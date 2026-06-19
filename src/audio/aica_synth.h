#ifndef AICA_SYNTH_H
#define AICA_SYNTH_H

/*
 * AICA hardware-mixed voice driver (Dreamcast) for SM64 (VERSION_US).
 * Replaces the software synthesis render path: maps each active gNotes[] voice
 * to an AICA hardware channel via the KOS command queue. Ported from the
 * MK64/OoT EU drivers, adapted to the US audio engine (struct Note / gNotes,
 * note->frequency, note->sound).
 *
 *   AicaSynth_Update()       : call from synthesis_execute, after the
 *                              process_sequences loop (lifecycle, once/frame).
 *   AicaSynth_RefreshActive(): call inside the sub-update loop, after each
 *                              process_sequences, to keep sub-frame pan/vol/freq
 *                              resolution (fast pan/tremolo/vibrato).
 *   AicaSynth_Init()         : call once at the end of audio_init() (uploads the
 *                              synthetic wavetables, anchors the linked pool).
 */

void AicaSynth_Init(void);
void AicaSynth_Update(void);
void AicaSynth_RefreshActive(void);

#endif /* AICA_SYNTH_H */
