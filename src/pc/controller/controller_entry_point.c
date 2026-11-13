#include "macros.h"

#include "lib/src/libultra_internal.h"
#include "lib/src/osContInternal.h"

#include "controller_dc.h"

#include <stdio.h>
#include "game/tas_slice.h"
static void tas_load(void);
static unsigned char *sTas;
static unsigned int sTasSamples, sTasNext;
static int sTasOn;   // the movie plays the game, not the controller

static struct ControllerAPI *controller_implementations[] = {
    &controller_dc,
};

s32 osContInit(UNUSED OSMesgQueue *mq, u8 *controllerBits, UNUSED OSContStatus *status) {
    size_t i;

    for (i = 0; i < sizeof(controller_implementations) / sizeof(struct ControllerAPI *); i++) {
        controller_implementations[i]->init();
    }
    tas_load();
    *controllerBits = 1;
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue *mesg) {
    return 0;
}

// The debug menu's TAS replay: a Mupen64 movie (tas.m64 in DC_TAS_ROOT, the disc's /afx; version
// 3, from power-on) plays instead of the controller. SM64 reads the controller once per game frame and a movie holds one sample per read
// (lag frames have none), so sample n is game frame n. A sample is the buttons in OSContPad's
// order (high byte A B Z START U D L R; low byte .. L R C-U C-D C-L C-R), then stick x and y.
#include <stdio.h>
#include <stdlib.h>
#ifndef DC_TAS_ROOT
#define DC_TAS_ROOT "/cd/afx"
#endif

static void tas_load(void) {
    const char *path = DC_TAS_ROOT "/tas.m64";
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("TAS: no %s; the controller plays\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sTas = size > 0x400 ? malloc((size_t) size) : NULL;
    if (sTas == NULL || fread(sTas, 1, (size_t) size, f) != (size_t) size || sTas[0] != 'M' || sTas[3] != 0x1A) {
        printf("TAS: cannot read the movie\n");
        free(sTas);
        sTas = NULL;
    } else {
        sTasSamples = (unsigned int) ((size - 0x400) / 4);
        printf("TAS: %u samples, start type %u\n", sTasSamples, sTas[0x1C] | sTas[0x1D] << 8);
    }
    fclose(f);
    if (sTas == NULL) {
        return;
    }
    // The slices (src/game/tas_slice.c): the debug menu picks what plays.
    tas_slice_setup(DC_TAS_ROOT "/tas_slices.bin");
}

// The game frame the movie has reached.
unsigned int dc_tas_frame(void) {
    return sTasNext;
}
unsigned int dc_tas_samples(void) {
    return sTasSamples;
}

// TAS slices (src/game/tas_slice.c): play on from another sample.
void dc_tas_seek(unsigned int sample) {
    sTasNext = sample;
    sTasOn = 1;
}

// The debug menu's replay is over (tas_slice.c): the controller plays again.
void dc_tas_stop(void) {
    sTasOn = 0;
}

// The movie plays the game, not the controller (no rumble then: controller_dc.c).
int dc_tas_playing(void) {
    return sTas != NULL && sTasOn;
}

// Show star (tas_slice.c): the player's pad reads since the level's start, logged, and played back
// fast to bring the player's game back after the replay.
extern void tas_own_pad(const OSContPad *pad);
extern s32 tas_own_fast_pad(OSContPad *pad);

void osContGetReadData(OSContPad *pad) {
    size_t i;

    pad->button = 0;
    pad->stick_x = 0;
    pad->stick_y = 0;
    pad->errnum = 0;
    extern void controller_dc_fsaa_poll(void);
    controller_dc_fsaa_poll();   // the FSAA toggle button, also under a movie
    if (tas_own_fast_pad(pad)) {
        return;
    }
    if (sTasOn && sTasNext >= sTasSamples) {
        sTasOn = 0;   // the whole run is over, after the credits: the controller plays on
    }
    if (sTas != NULL && sTasOn) {
        if (sTasNext < sTasSamples) {
            const unsigned char *in = sTas + 0x400 + sTasNext * 4;
            pad->button = (u16) (in[0] << 8 | in[1]);
            pad->stick_x = (s8) in[2];
            pad->stick_y = (s8) in[3];
            if (++sTasNext % 1800 == 0) printf("TAS: frame %u\n", sTasNext);
        }
        return;
    }

    for (i = 0; i < sizeof(controller_implementations) / sizeof(struct ControllerAPI *); i++) {
        controller_implementations[i]->read(pad);
    }
    tas_own_pad(pad);
}
