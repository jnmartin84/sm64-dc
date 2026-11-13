#ifndef DC_VIDEO_OPTIONS_H
#define DC_VIDEO_OPTIONS_H
/* The video options a player can pick in the debug menu, as two independent settings.
 *
 * FSAA (gDcFsaaMode, gfx_pvr.c). The PVR's FSAA mode changes between scenes, or
 * from the debug menu or a toggle button (it takes a PVR restart). */
#define DC_FSAA_MODE_OFF      0   /* never */
#define DC_FSAA_MODE_ON       1   /* always */
#define DC_FSAA_MODE_CURATED  2   /* always, except DC_FSAA_OFF_SCENES (water, lava, heavy fill) */
#define DC_FSAA_MODE_TOGGLE_X 3   /* a press of X in play switches it (controller_dc.c), and only B
                                     is the N64's B; it stays as it is across scenes */
#define DC_FSAA_MODE_TOGGLE_B 4   /* the same with B, and only X is the N64's B */
#ifndef DC_FSAA_MODE
#define DC_FSAA_MODE DC_FSAA_MODE_CURATED
#endif
extern int gDcFsaaMode;

/* Resolution (gDcVideo240p, gfx_pvr.c): 0 = 640x480 (interlaced on a TV, 480p on VGA), 1 = 320x240,
 * progressive 240p on a TV (line-doubled on VGA). The debug menu switches it (a PVR restart).
 * make LOWRES=1 starts in 240p. */
#ifndef DC_VIDEO_240P
#ifdef LOWRES
#define DC_VIDEO_240P 1
#else
#define DC_VIDEO_240P 0
#endif
#endif
extern int gDcVideo240p;

/* Frame rate (gDcFrameMode, pc_main.c). Each 30 Hz logic step is shown as an
 * interpolated frame and then the exact one. */
#define DC_FRAME_MODE_30      0   /* the exact frame only, for two vblanks */
#define DC_FRAME_MODE_60      1   /* both; a late frame delays all that follow (the game slows) */
#define DC_FRAME_MODE_60_DROP 2   /* both; when the interpolated frame is late, the exact one is
                                     dropped, so the step keeps its two vblanks and game time keeps up */
#ifndef DC_FRAME_MODE
#define DC_FRAME_MODE DC_FRAME_MODE_60_DROP
#endif
extern int gDcFrameMode;
#endif
