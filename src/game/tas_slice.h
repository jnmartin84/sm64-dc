#ifndef TAS_SLICE_H
#define TAS_SLICE_H

#include <PR/ultratypes.h>

void tas_slice_setup(const char *sliceFile);
s32 tas_slice_first_level(s32 levelNum);
s32 tas_slice_warp(s16 *destLevel, s16 *destArea, s16 *destWarpNode, s32 *arg);
void tas_slice_init_level(void);

/* The debug menu's TAS row (pc_main.c): entry 0 gives the game back to the controller, 1 plays the
 * whole run, the others one section each (a slice that collects a star or two, or a Bowser). */
s32 tas_menu_count(void);
const char *const *tas_menu_labels(void);
const char *tas_menu_help(s32 entry);   /* the entry's course, stars and times; a static buffer */
void tas_menu_start(s32 entry);
s32 tas_ending_wait(s16 arg, s32 reg);   /* "The End"'s level script: nonzero leaves for the title */
/* The level select row: entry 0 is Off, the others each star (and Bowser fight) by course, for the
 * controller to play from the movie's state when it entered that level for it. */
s32 tas_level_count(void);
const char *const *tas_level_labels(void);
const char *tas_level_help(s32 entry);
void tas_level_start(s32 entry);
/* Show star: the TAS getting a star of this course, then the player's game where it was. */
const char *const *tas_show_labels(s32 *count, s32 *selected);
const char *tas_show_help(s32 value);
s32 tas_show_start(s32 value);
s32 tas_show_active(void);
s32 tas_show_fast(void);      /* the way back plays: the frames are not drawn */
s32 tas_show_percent(void);
void tas_show_update(void);   /* update_level, once a frame */

#endif
