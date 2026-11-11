#ifndef GFX_DC_TAGS_H
#define GFX_DC_TAGS_H
/* Dreamcast-only display-list tags (gDPNoOpTag; gfx_retro_dc.c reads them, the N64 RDP ignores
 * G_NOOP). A COVER is solid opaque black over the whole frame outside a transition's hole
 * (screen_transition.c): drawn into the PVR's OP list above every world depth, it hides what lies
 * under it from the PT and TR lists too, so their fill is not paid behind black. */
#define DC_GFX_COVER_BEGIN 0x434F5642u   /* "COVB" */
#define DC_GFX_COVER_END   0x434F5645u   /* "COVE" */
/* In a static list: it is never compiled, but runs command by command. The compiled form has no 2D depth, and
 * a transition's soft edge (screen_transition_mesh.h) must lie over the scene's TR list. */
#define DC_GFX_NO_COMPILE   0x494E5450u   /* "INTP" */
/* The skybox (bin/segment2.c dl_skybox_begin .. dl_skybox_end): its G_RM_OPA_SURF triangles are
 * opaque and go to the OP list. Only there: goddard's Mario head and the screen borders use the same
 * render mode and need the TR path (the head's highlight blends there). */
#define DC_GFX_SKY_BEGIN 0x534B5942u     /* "SKYB" */
#define DC_GFX_SKY_END   0x534B5945u     /* "SKYE" */
/* BBH's chain-link fence (levels/bbh/areas/1/36) goes to the TR list instead of
 * PT. Measured 2026-10-24 (FSAA, BBH demo x5): PVR-late 110 -> 0, PVR avg 10.0 -> 9.1 ms, max
 * 20.2 -> 16.0 ms; drawn in PT it was the course's biggest fill (134k pixels a frame). */
#define DC_GFX_RAIL_TR_BEGIN 0x5241494Cu  /* "RAIL" */
#define DC_GFX_RAIL_TR_END   0x5241494Eu  /* "RAIN" */
/* Fades: a fade is a full-screen rectangle of one colour at partial alpha over all that
 * was drawn before it (screen_transition.c dl_transition_color, ingame_menu.c shade_screen). On the
 * Dreamcast it is not drawn: the PVR's fog mixes its colour into every polygon drawn before it
 * instead (gfx_pvr.c), which costs no fill. The master display list starts with two G_NOOPs
 * (dc_fade_frame_begin): DC_GFX_FADE and the colour and alpha as 0xRRGGBBAA when the frame has a
 * fade, else two plain G_NOOPs. DC_GFX_FADE_END stands where the rectangle was: the fade ends there. */
#define DC_GFX_FADE      0x46414445u   /* "FADE" */
#define DC_GFX_FADE_END  0x46414458u   /* "FADX" */
#endif
