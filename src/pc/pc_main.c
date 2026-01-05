#include <stdlib.h>

#ifdef TARGET_WEB
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"
#include "gfx/gfx_opengl.h"
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

void send_display_list(struct SPTask *spTask) {
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#include <kos.h>
#undef bool
#undef true
#undef false
#define bool uint8_t
#define false 0
#define true 1
//void create_thread(OSThread *thread, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri);
static volatile uint64_t vblticker=0;
void vblfunc(uint32_t c, void *d) {
	(void)c;
	(void)d;
    vblticker++;
    genwait_wake_one((void*)&vblticker);
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
            genwait_wait((void*)&vblticker, NULL, 3, NULL);
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

void main_func(void) {
    main_pc_pool = &pool;
    main_pc_pool_gd = &pool[0x165000];

    main_pool_init(pool, pool + sizeof(pool) / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    configfile_load(CONFIG_FILE);
    atexit(save_config);

    rendering_api = &gfx_opengl_api;
    wm_api = &gfx_dc;

    gfx_init(wm_api, rendering_api, "Super Mario 64", configFullscreen);
    
    wm_api->set_fullscreen_changed_callback(on_fullscreen_changed);
    
    if (audio_api == NULL && audio_dc.init()) {
        audio_api = &audio_dc;
    }

    if (audio_api == NULL) {
        audio_api = &audio_null;
    }

    audio_init();
    sound_init();

    vblank_handler_add(&vblfunc, NULL);
    kthread_attr_t audio_attr;
    audio_attr.create_detached = 1;
	audio_attr.stack_size = 32768;
	audio_attr.stack_ptr = NULL;
	audio_attr.prio = PRIO_DEFAULT + 1;
	audio_attr.label = "AudioSynthesis";
    thd_create_ex(&audio_attr, &AudioSynthesisThread, NULL);

    thread5_game_loop(NULL);

    inited = 1;

    while (1) {
        gfx_start_frame();
        game_loop_one_iteration();
        gfx_end_frame();
        gSysFrameCount++;
    }
}


#include <kos.h>
int main(UNUSED int argc, UNUSED char *argv[]) {
    mmu_init_basic();
    mmu_page_map_static(0, 0x0C000000, PAGE_SIZE_1M, MMU_ALL_RDWR, true);

    main_func();
    return 0;
}
