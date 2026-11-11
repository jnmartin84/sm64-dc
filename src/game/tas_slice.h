#ifndef TAS_SLICE_H
#define TAS_SLICE_H

#include <PR/ultratypes.h>

void tas_slice_setup(const char *sliceFile);
s32 tas_slice_first_level(s32 levelNum);
void tas_slice_warp(s16 *destLevel, s16 *destArea, s16 *destWarpNode, s32 *arg);
void tas_slice_init_level(void);

/* The debug menu's TAS row (pc_main.c): entry 0 gives the game back to the controller, 1 plays the
 * whole run, the others one section each (a slice that collects a star or two, or a Bowser). */
s32 tas_menu_count(void);
const char *const *tas_menu_labels(void);
const char *tas_menu_help(s32 entry);   /* the entry's course, stars and times; a static buffer */
void tas_menu_start(s32 entry);
/* The level select row: entry 0 is Off, the others each star (and Bowser fight) by course, for the
 * controller to play from the movie's state when it entered that level for it. */
s32 tas_level_count(void);
const char *const *tas_level_labels(void);
const char *tas_level_help(s32 entry);
void tas_level_start(s32 entry);

#endif
