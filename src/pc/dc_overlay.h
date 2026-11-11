#ifndef DC_OVERLAY_H
#define DC_OVERLAY_H
/* Frame rate and missed frames on screen (dc_overlay.c), turned on and off in the debug menu
 * (pc_main.c). */
extern int gDcOverlayOn;            /* shown (off at start) */
#define DC_OVERLAY_SHOWN gDcOverlayOn
void dc_overlay_count_draw(void);   /* pc_main.c, after each drawn picture */
void dc_overlay_count_skip(void);   /* pc_main.c, an exact frame skipped after a late one */
void dc_overlay_draw(void);         /* gfx_pvr.c, with the TR list open */
void dc_overlay_pvr_reset(void);    /* gfx_pvr.c, after a PVR restart */
int dc_overlay_font_ready(void);    /* the quick font in video memory, for the debug menu too */
#endif
