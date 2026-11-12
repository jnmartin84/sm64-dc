#include <stdlib.h>

#ifdef TARGET_WEB
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"
#include "gfx/gfx_dc.h"

#include "audio/audio_api.h"
#include "audio/audio_dc.h"
#include "audio/audio_null.h"

#include "configfile.h"

#include "compat.h"
uint64_t gSysFrameCount = 0;

#undef CONT_C
#undef CONT_B
#undef CONT_A
#undef CONT_START
#undef CONT_DPAD_UP
#undef CONT_DPAD_DOWN
#undef CONT_DPAD_LEFT
#undef CONT_DPAD_RIGHT
#undef CONT_Z
#undef CONT_Y
#undef CONT_X
#undef CONT_D
#undef CONT_DPAD2_UP
#undef CONT_DPAD2_DOWN
#undef CONT_DPAD2_LEFT
#undef CONT_DPAD2_RIGHT


#define CONFIG_FILE "sm64config.txt"

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

static struct AudioAPI *audio_api;
static struct GfxWindowManagerAPI *wm_api;
static struct GfxRenderingAPI *rendering_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);

extern void create_next_audio_buffer(s16 *samplesL, s16* samplesR, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {
}

static uint8_t inited = 0;

#include "game/game_init.h" // for gGlobalTimer
#include "pc/dc_overlay.h"

#include "pc/dc_video_options.h"
#include <dc/pvr.h>
int gDcFrameMode = DC_FRAME_MODE;
extern unsigned int gfx_dc_vbl_late;

// DC_FRAME_MODE_60_DROP: after the step's first (interpolated) frame was handed over and paced to
// its vblank, is it late? Late if it was handed over after that vblank (CPU), or if the PVR's last
// render took longer than a frame: render times hold from frame to frame, so this one will too.
// (Counting KOS's flips instead does not work: two frames are normally in flight at this point.)
#ifndef DC_LATEDROP_RENDER_US
#define DC_LATEDROP_RENDER_US 16700
#endif
static int first_frame_late(unsigned int late_before) {
    pvr_stats_t st;
    pvr_get_stats(&st);
    return gfx_dc_vbl_late != late_before || st.rnd_last_time > DC_LATEDROP_RENDER_US * 1000ull;
}

static int sFrameQueued;   // send_display_list ran in this frame (the enDjinn loop below)

void send_display_list(struct SPTask *spTask) {
    sFrameQueued = 1;
    if (gDcFrameMode == DC_FRAME_MODE_30) {   // the exact matrices now; the second draw then finds
        extern s32 mtx_patch_interpolated(void);   // nothing to change and holds this frame
        mtx_patch_interpolated();
    }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#include <kos.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>
#include <enDjinn/enj_debug_menu.h>
#undef bool
#undef true
#undef false
#define bool uint8_t
#define false 0
#define true 1
//void create_thread(OSThread *thread, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri);
volatile uint64_t vblticker=0;
void vblfunc(uint32_t c, void *d) {
	(void)c;
	(void)d;
    vblticker++;
    genwait_wake_all/* one */((void*)&vblticker);
    //thd_schedule(false, 0);
}    

#define SAMPLES_HIGH 448 
//464
//#define SAMPLES_LOW 448
//432
s16 audio_buffer[2][SAMPLES_HIGH * 2 * 2 * 3] __attribute__((aligned(64)));


void *AudioSynthesisThread(UNUSED void *arg) {
    uint64_t last_vbltick = vblticker;

    while (1) {
        while (vblticker <= last_vbltick)
#ifdef KOS_GENWAIT_3ARG
            genwait_wait((void*)&vblticker, NULL, 0);
#else
            genwait_wait((void*)&vblticker, NULL, 0/* 3 */, NULL);
#endif
        last_vbltick = vblticker;
// if you notice the sound starts skipping, re-enable the irq_disable/enable around synthesis
//        irq_disable();
        // num samples is 448
        create_next_audio_buffer(audio_buffer[0], audio_buffer[1], 448);
        audio_api->play((u8 *)audio_buffer[0], (u8 *)audio_buffer[1], 1792);
//        irq_enable();
    }
    return NULL;
}

extern int gProcessAudio;
void produce_one_frame(void) {
    gfx_start_frame();
    game_loop_one_iteration();
    gfx_end_frame();
}

static void save_config(void) {
    configfile_save(CONFIG_FILE);
}

static void on_fullscreen_changed(bool is_now_fullscreen) {
    configFullscreen = is_now_fullscreen;
}

void *main_pc_pool = NULL;
void *main_pc_pool_gd = NULL;

static u8 pool[0x165000+0x70800] __attribute__((aligned(16384)));

// enDjinn's enj_state_run is the frame loop. Each of its frames is one scene of the game's: a logic
// step, whose display list send_display_list queues as the opaque list's renderer (gfx_pvr.c), or at
// 60 fps the step's second, exact draw of that list, or the last scene shown again. enDjinn then
// submits the scene, and its post-render callback ends the frame as the plain loop in main_func does
// after each draw: the scene's end, the pacing to its vblank, the profiler's frame.
extern void gfx_dc_hold_frame(void);
static int sFrameHeld;    // this frame shows the last scene again
static int sStepped;      // a logic step has run
static int sExactNext;    // the next frame is the step's exact draw,
static int sDropExact;    // unless the interpolated one was late (DC_FRAME_MODE_60_DROP)
static unsigned int sLate0;

static void enj_frame_step(void) {
    if (sStepped) {
        thd_pass();   // between steps, as the plain loop
    }
    sStepped = 1;
    sLate0 = gfx_dc_vbl_late;
    sExactNext = 1;
    sFrameQueued = 0;
    gfx_start_frame();
    game_loop_one_iteration();
    sFrameHeld = !sFrameQueued;   // nothing drawn (the reset bars): the screen stays
    if (sFrameHeld) {
        enj_render_hold_frame();
    }
}

static void enj_frame(UNUSED void *data) {
    if (sExactNext) {
        extern s32 mtx_patch_interpolated(void);
        sExactNext = 0;
        if (sDropExact) {
            mtx_patch_interpolated();   // still empties its table (it points into this display list)
            dc_overlay_count_skip();
        } else if (!gResetTimer) {
            // When the exact matrices equal the interpolated ones the second draw would repeat the
            // first: show that frame again.
            sFrameHeld = !mtx_patch_interpolated();
            if (sFrameHeld) {
                enj_render_hold_frame();
            } else {
                gfx_start_frame();
                send_display_list(gGfxSPTask);
            }
            return;
        }
    }
    enj_frame_step();
}

// enDjinn's debug menu (third_party/endjinn enj_debug_menu.h): hold both triggers and press
// Up+X+Y. It is a mode on top of the game's, so no logic step runs while it is open; behind it the
// game's last display list is drawn again as it was last drawn, and those frames are the menu's,
// not the game's (enj_frame_shown). The quick font it draws with is the overlay's (dc_overlay.c),
// which a PVR restart drops and loads again.

static int sMenuFrame;   // this frame is the one behind the menu

static void dbg_menu_behind(UNUSED void *data) {
    sMenuFrame = 1;
    dc_overlay_font_ready();
    if (sStepped && gGfxSPTask != NULL) {   // else nothing has been drawn: the background colour
        gfx_run((Gfx *) gGfxSPTask->task.t.data_ptr);
    }
}

static const char *const sDbgFrameModes[] = { "30 fps", "60 fps", "60 fps, drops" };
extern int gDcFsaaMode;
extern void gfx_pvr_fsaa_mode_changed(void);
static const char *const sDbgFsaaModes[] = { "Off", "On", "Auto", "Toggle X button", "Toggle B button" };

static void dbg_fsaa_changed(UNUSED enj_menu_row_t *row, UNUSED void *data) {
    gfx_pvr_fsaa_mode_changed();   // the PVR restarts before the game's next frame
}
static const char *const sDbgOffOn[] = { "Off", "On" };
#include "pc/dc_video_options.h"
static const char *const sDbgResolutions[] = { "640x480", "320x240 (240p)" };
static int sDbgVideo240p = DC_VIDEO_240P;

// The PVR restarts in the other video mode before the game's next frame (gfx_pvr.c), after the
// menu closes: the restart frees the VRAM the menu draws from.
static void dbg_resolution_changed(UNUSED enj_menu_row_t *row, UNUSED void *data) {
    gDcVideo240p = sDbgVideo240p;
}
#include "audio/sm64_aicaflow.h"

static void dbg_reverb_changed(UNUSED enj_menu_row_t *row, UNUSED void *data) {
    sm64_afx_reverb_returns();
}
#include "game/save_file.h"

// Save: 120 stars plays with a finished game's save, kept in memory (ultra_reimplementation.c);
// VMU gives the player's own back. Either way the game goes to the title, as Save & Quit does.
extern void dc_star_save_set(s32 on);
extern void tas_menu_reset_game(void);   // level_update.c
static const char *const sDbgSaves[] = { "VMU", "120 stars" };
static int sDbgStarSave;

static void dbg_save_changed(UNUSED enj_menu_row_t *row, UNUSED void *data) {
    dc_star_save_set(sDbgStarSave);
    save_file_load_all();
    tas_menu_reset_game();
}
#include "game/tas_slice.h"

static int sDbgTas;

static void dbg_tas_changed(enj_menu_row_t *row, UNUSED void *data) {
    row->help = tas_menu_help(sDbgTas);
}

// A starts the replay: the game warps when the menu closes, as its logic runs again.
static void dbg_tas_pressed(enj_menu_row_t *row, UNUSED void *data) {
    tas_menu_start(sDbgTas);
    row->help = sDbgTas ? "Starts when the menu closes: at once in a level, else at the file's "
                          "first level. Off and A: the pad plays again, from your VMU save."
                        : tas_menu_help(0);
}

static int sDbgLevel;

static void dbg_level_changed(enj_menu_row_t *row, UNUSED void *data) {
    row->help = tas_level_help(sDbgLevel);
}

static void dbg_level_pressed(enj_menu_row_t *row, UNUSED void *data) {
    tas_level_start(sDbgLevel);
    if (sDbgLevel) {
        row->help = "Starts when the menu closes: at once in a level, else at the file's first level.";
    }
}

// The player's video options (dc_video_options.h) and the overlay.
static enj_menu_row_t sDbgRows[] = {
    { .name = "Frame rate",
      .help = "30 fps shows each logic step for two refreshes. 60 fps adds a frame between steps: "
              "a late one slows the game, or with drops it is skipped.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcFrameMode, .labels = sDbgFrameModes, .count = 3 } },
    { .name = "FSAA",
      .help = "Twice the width, filtered down. Auto: off in water, lava and the heaviest scenes. Toggle: "
              "that button switches it in play, the other of B and X stays B. Each switch loads "
              "the textures again.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcFsaaMode, .labels = sDbgFsaaModes, .count = 5 },
      .on_change = dbg_fsaa_changed,
      .apply_on_close = 1 },
    { .name = "Resolution",
      .help = "320x240 is the N64's own: progressive 240p on a TV, line-doubled on VGA. "
              "640x480 is interlaced on a TV. Each switch loads the textures again.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &sDbgVideo240p, .labels = sDbgResolutions, .count = 2 },
      .on_change = dbg_resolution_changed,
      .apply_on_close = 1 },
    { .name = "Reverb",
      .help = "The N64's echo, as the AICA DSP's program. Off silences its output; the program "
              "runs on. Changes at once.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcReverbOn, .labels = sDbgOffOn, .count = 2 },
      .on_change = dbg_reverb_changed },
    { .name = "Show FPS",
      .help = "The frame rate, late frames (MISS) and skipped ones (SKIP) of the scene, in the "
              "bottom left corner.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcOverlayOn, .labels = sDbgOffOn, .count = 2 } },
    { .name = "Save",
      .help = "120 stars: a finished game's save, kept in memory: nothing is written to the VMU, and "
              "each switch to it starts it afresh. VMU: your own. Either goes back to the title.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &sDbgStarSave, .labels = sDbgSaves, .count = 2 },
      .on_change = dbg_save_changed,
      .apply_on_close = 1 },
    // Last: left out without the movie's files. Labels and counts come from tas_slice.c.
    { .name = "TAS replay",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &sDbgTas },
      .on_change = dbg_tas_changed,
      .on_press = dbg_tas_pressed },
    { .name = "Level select",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &sDbgLevel },
      .on_change = dbg_level_changed,
      .on_press = dbg_level_pressed },
};

static enj_debug_menu_t sDbgMenu = {
    .title = "Super Mario 64",
    .rows = sDbgRows,
    .row_count = sizeof(sDbgRows) / sizeof(sDbgRows[0]),
    .draw_behind = dbg_menu_behind,
};

static void enj_frame_shown(UNUSED void *data) {
    if (sMenuFrame) {   // behind the debug menu: shown and paced, but none of the game's frames
        sMenuFrame = 0;
        gfx_end_frame();
        return;
    }
    if (sFrameHeld) {
        gfx_dc_hold_frame();
    } else {
        gfx_end_frame();
        dc_overlay_count_draw();
    }
    if (sExactNext) {   // the step's own frame: decide its exact draw now, as the plain loop does
        sDropExact = gDcFrameMode == DC_FRAME_MODE_60_DROP && first_frame_late(sLate0);
    }
    gSysFrameCount++;
}

static void enj_loop_run(void) {
    static enj_mode_t sGameMode;
    sGameMode.mode_updater = enj_frame;
    sGameMode.no_soft_reset = 1;
    strncpy(sGameMode.name, "sm64", sizeof(sGameMode.name) - 1);
    enj_render_post_callback_set(enj_frame_shown, NULL);
    enj_mode_push(&sGameMode);
    if (enj_state_get()->flags.no_input_init) {
        // The menu's buttons are read through enDjinn's controller layer, which the game's own
        // maple code (controller_dc.c) leaves unstarted.
        extern void enj_ctrl_init_local_devices(void);
        enj_ctrl_init_local_devices();
    }
    enj_menu_row_t *tas = &sDbgRows[sizeof(sDbgRows) / sizeof(sDbgRows[0]) - 2];
    tas[0].choice.labels = tas_menu_labels();
    tas[0].choice.count = tas_menu_count();
    tas[0].help = tas_menu_help(0);
    tas[1].choice.labels = tas_level_labels();
    tas[1].choice.count = tas_level_count();
    tas[1].help = tas_level_help(0);
    if (tas[0].choice.count == 0) {
        sDbgMenu.row_count -= 2;   // no tas.m64 and tas_slices.bin
    }
    enj_debug_menu_set(&sDbgMenu);
    enj_state_run();
}

void main_func(void) {
    main_pc_pool = &pool;
    main_pc_pool_gd = &pool[0x165000];

    main_pool_init(pool, pool + sizeof(pool) / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    configfile_load(CONFIG_FILE);
    atexit(save_config);

    extern struct GfxRenderingAPI gfx_pvr_api;
    rendering_api = &gfx_pvr_api;   // make GFX_BACKEND=pvr
    wm_api = &gfx_dc;

    gfx_init(wm_api, rendering_api, "Super Mario 64", configFullscreen);
    
    wm_api->set_fullscreen_changed_callback(on_fullscreen_changed);
    
    if (audio_api == NULL) {
        audio_api = &audio_null;
    }

    audio_init();
    sound_init();

    vblank_handler_add(&vblfunc, NULL);
    // No sequencer on the SH4: the music and sounds are AICAflow flows (src/pc/audio/sm64_aicaflow.c).
    {
        extern int sm64_afx_init(void);
        sm64_afx_init();
    }
//    (void)audio_attr;
    thread5_game_loop(NULL);

    inited = 1;

    enj_loop_run();
}


#include <kos.h>
int main(UNUSED int argc, UNUSED char *argv[]) {
    mmu_init_basic();
    mmu_page_map_static(0, 0x0C000000, PAGE_SIZE_1M, MMU_ALL_RDWR, true);

    main_func();
    return 0;
}
