#ifndef SM64_AICAFLOW_H
#define SM64_AICAFLOW_H
/* The game's music and sounds play as AICAflow assets (src/pc/audio/sm64_aicaflow.c) instead of
 * through the N64 sequencer. Assets come from tools/aica/sm64_audio_assets.sh. */
#include <PR/ultratypes.h>

int sm64_afx_init(void);
/* The music players: external.c keeps the original's queue and volumes in the sequence players,
 * and plays a player's song here at a gain over the volume it was recorded at. */
int sm64_afx_play_sequence(u8 player, u8 seqId);
int sm64_afx_music_playing(u8 player);
void sm64_afx_music_gain(u8 player, f32 gain);
/* The song's sequence channels, as its flow's lanes: those it started, at their volume scales. */
void sm64_afx_music_channels(u8 player, u16 enabled, const f32 *scale);
void sm64_afx_music_stop(u8 player);
/* The sound channels: one per sound bank, as update_game_sound chooses what they play. */
void sm64_afx_sfx_start(u8 channel, u32 soundBits);
int sm64_afx_sfx_active(u8 channel);
void sm64_afx_sfx_stop(u8 channel);
void sm64_afx_sfx_place(u8 channel, u32 soundBits, f32 volume, f32 pan, f32 pitch, int echo);
void sm64_afx_sfx_fade(u8 channel, u16 ticks);
/* The pause menu (set_sound_disabled), sound_reset, and the file select's mono option. */
void sm64_afx_mute(u8 muted);
void sm64_afx_stop_all(void);
void sm64_afx_mono(u8 mono);
void sm64_afx_tick(void);
/* level_update.c init_level: read the level's scene pack (and the core pack, once) before the
 * level starts, so its songs and sounds do not load from /pc mid-scene. */
void sm64_afx_scene_prepare(s16 level);
/* level_select_menu.c: the title is on screen. After a demo gCurrLevelNum is still the demo's
 * level, and Mario's "press start" comes before the title's song does. */
void sm64_afx_menu_enter(void);
/* external.c sound_reset: the session preset's reverb (N64 ring length in 32 kHz samples, and
 * feedback, Q15) becomes the scene's DSP program. */
void sm64_afx_reverb(u16 windowSize, u16 gain);
/* The debug menu's reverb switch: the DSP program keeps running, its output to the mix is gated
 * (sm64_afx_reverb_returns applies gDcReverbOn at once, and each new program keeps it). */
extern int gDcReverbOn;
void sm64_afx_reverb_returns(void);
/* external.c: a sound's reverb send (0..127) from its script's value and its volume. */
u8 sm64_sound_reverb(u32 soundBits, f32 volume, u8 scriptReverb);
/* external.c: sm64_afx_reverb with an audio session preset's values. */
void sm64_sound_reverb_preset(u8 presetId);
#endif
