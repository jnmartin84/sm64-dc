#ifndef AICA_SYNTH_H
#define AICA_SYNTH_H

/*
 * AICA hardware-mixed voice driver (Dreamcast) for SM64 (VERSION_US).
 * Replaces the software synthesis render path: maps each active gNotes[] voice
 * to an AICA hardware channel via the KOS command queue. Ported from the
 * MK64/OoT EU drivers, adapted to the US audio engine (struct Note / gNotes,
 * note->frequency, note->sound).
 *
 *   AicaSynth_Update() : call from synthesis_execute, after the
 *                        process_sequences loop (replaces the render).
 *   AicaSynth_Init()   : call once at the end of audio_init() (uploads the
 *                        synthetic wavetables, anchors the linked sample pool).
 */

void AicaSynth_Init(void);
void AicaSynth_Update(void);

#endif /* AICA_SYNTH_H */
