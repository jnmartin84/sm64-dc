#include <stdbool.h>
#include <PR/ultratypes.h>
//#include <ultra64.h>
/* Dreamcast and Nintendo share these defines... the odds... Copy it here */
/* Buttons */

#define N64_CONT_A 0x8000
#define N64_CONT_B 0x4000
#define N64_CONT_G 0x2000
#define N64_CONT_START 0x1000
#define N64_CONT_UP 0x0800
#define N64_CONT_DOWN 0x0400
#define N64_CONT_LEFT 0x0200
#define N64_CONT_RIGHT 0x0100
#define N64_CONT_L 0x0020
#define N64_CONT_R 0x0010
#define N64_CONT_E 0x0008
#define N64_CONT_D 0x0004
#define N64_CONT_C 0x0002
#define N64_CONT_F 0x0001

/* Nintendo's official button names */

#define A_BUTTON N64_CONT_A
#define B_BUTTON N64_CONT_B
#define L_TRIG N64_CONT_L
#define R_TRIG N64_CONT_R
#define Z_TRIG N64_CONT_G
#define START_BUTTON N64_CONT_START
#define U_JPAD N64_CONT_UP
#define L_JPAD N64_CONT_LEFT
#define R_JPAD N64_CONT_RIGHT
#define D_JPAD N64_CONT_DOWN
#define U_CBUTTONS N64_CONT_E
#define L_CBUTTONS N64_CONT_C
#define R_CBUTTONS N64_CONT_F
#define D_CBUTTONS N64_CONT_D

typedef struct {
    u16 button;
    s8 stick_x; /* -80 <= stick_x <= 80 */
    s8 stick_y; /* -80 <= stick_y <= 80 */
    u8 errnum;
} OSContPad;

/* End Copying this here */

//#include "controller_api.h"
/* Replace this because of Ultra64 Incompatibilities */
struct ControllerAPI {
    void (*init)(void);
    void (*read)(OSContPad *pad);
};
#include <dc/maple.h>
#include <dc/maple/controller.h>

// Rumble (src/game/thread6.c): the game switches the Rumble Pak's one motor on and off at each of
// the N64's vertical blanks, and sets a strength by the share of them it is on. That share over the
// last eight becomes the Puru Puru pack's strength, 1 to 7, sent only when it moves by two steps or
// the motor starts or stops, so maple sees changes only. Diddy Kong Racing's port does the same.
// Neither a TAS replay nor the title's demos rumble: nobody holds the controller then.
#include <dc/maple/purupuru.h>
#include <enDjinn/enj_rumble.h>

#define RUMBLE_FREQ 26   // the Puru Puru's vibration frequency setting

extern void enj_rumble_init_local_devices(void);

static uint8_t sRumbleHistory;   // one bit per vertical blank, the newest lowest
static uint8_t sRumbleSent;      // the strength sent last, 0 for stopped

static void rumble_send(int strength) {
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);   // the game's controller
    if (cont != NULL) {
        purupuru_effect_t effect = { .motor = 1 };   // a raw 0 would not be sent: this stops it
        if (strength != 0) {
            effect = (purupuru_effect_t) { .cont = 1, .motor = 1, .fpow = strength, .freq = RUMBLE_FREQ };
        }
        enj_rumble_effect_set(cont->port, effect);
    }
    sRumbleSent = (uint8_t) strength;
}

void controller_dc_rumble_motor(int on) {
    extern int dc_tas_playing(void);
    if (dc_tas_playing()) {
        on = 0;   // a replay rumbles nobody's hands
    }
    sRumbleHistory = (uint8_t) ((sRumbleHistory << 1) | (on != 0));
    int frames = __builtin_popcount(sRumbleHistory);
    int strength = frames == 0 ? 0 : 1 + ((frames - 1) * 6 + 3) / 7;   // 1..8 of 8 -> 1..7
    int sent = sRumbleSent;
    if (strength == sent || (strength != 0 && sent != 0 && strength - sent < 2 && sent - strength < 2)) {
        return;
    }
    rumble_send(strength);
}

// No game frames run (the debug menu is open): the motor stops now, not when the game goes on.
void controller_dc_rumble_stop(void) {
    sRumbleHistory = 0;
    if (sRumbleSent != 0) {
        rumble_send(0);
    }
}

static void controller_dc_init(void) {
    // enDjinn's loop sends the effects (enj_rumble_update); the game's maple code leaves its input
    // layer unstarted (no_input_init), so the packs are found here.
    enj_rumble_init_local_devices();
}

#include "pc/dc_video_options.h"
extern void gfx_pvr_fsaa_toggle(void);

// The debug menu's FSAA row can give X or B to FSAA (DC_FSAA_MODE_TOGGLE_X/_B): the other of the
// two plays the N64's B alone.
static uint32_t fsaa_button(void) {
    return gDcFsaaMode == DC_FSAA_MODE_TOGGLE_X ? CONT_X : gDcFsaaMode == DC_FSAA_MODE_TOGGLE_B ? CONT_B : 0;
}

// At every input read of the game: a press of the button switches
// FSAA, but not with both triggers held, where X is part of the debug menu's combination.
void controller_dc_fsaa_poll(void) {
    static uint32_t held;
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!cont)
        return;
    cont_state_t *state = (cont_state_t *) maple_dev_status(cont);
    uint32_t pressed = state->buttons & ~held;
    held = state->buttons;
    if ((pressed & fsaa_button()) && !((uint8_t) state->ltrig >= 128 && (uint8_t) state->rtrig >= 128))
        gfx_pvr_fsaa_toggle();
}

// n64 controls https://strategywiki.org/wiki/Super_Mario_64/Controls
// DC -> N64 control mapping
//  B, X     -> B (one of them toggles FSAA instead, if the debug menu says so)
//  A        -> A
//  Y        -> R Trigger
//  L Trig   -> Z Trigger
//  R Trig   -> Z Trigger
//  Analog   -> Move
//  Start    -> Start
//  DPad     -> Camera buttons
#define configDeadzone (0x20)

#include <stdlib.h>
static void controller_dc_read(OSContPad *pad) {
    maple_device_t *cont;
    cont_state_t *state;

    cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!cont)
        return;
    state = (cont_state_t *) maple_dev_status(cont);
#if 1
//FOR_DCLOAD
    if((state->buttons & CONT_A)  && (state->buttons & CONT_B) && (state->buttons & CONT_X) && (state->buttons & CONT_Y) && (state->buttons & CONT_START)) {
        exit(0);
    }
#endif
    const char stickH =state->joyx;
    const char stickV = 0xff-((uint8_t)(state->joyy));
    const uint32_t magnitude_sq = (uint32_t)(stickH * stickH) + (uint32_t)(stickV * stickV);

    if (magnitude_sq > (uint32_t)(configDeadzone * configDeadzone)) {
        pad->stick_x = ((float)stickH/127)*80;
        pad->stick_y = ((float)stickV/127)*80;
    }
    if (state->buttons & CONT_START)
        pad->button |= START_BUTTON;
    uint32_t b_buttons = CONT_B | CONT_X;
    b_buttons &= ~fsaa_button();
    if (state->buttons & b_buttons)
        pad->button |= B_BUTTON;
    if (state->buttons & CONT_A)
        pad->button |= A_BUTTON;
    if (state->buttons & CONT_Y)
        pad->button |= R_TRIG;
    if (((uint8_t) state->ltrig & 0x80 /* 128 */))
        pad->button |= Z_TRIG;
    if (((uint8_t) state->rtrig & 0x80 /* 128 */))
        pad->button |= Z_TRIG;
    if (state->buttons & CONT_DPAD_UP)
        pad->button |= U_CBUTTONS;
    if (state->buttons & CONT_DPAD_DOWN)
        pad->button |= D_CBUTTONS;
    if (state->buttons & CONT_DPAD_LEFT)
        pad->button |= L_CBUTTONS;
    if (state->buttons & CONT_DPAD_RIGHT)
        pad->button |= R_CBUTTONS;
}

struct ControllerAPI controller_dc = { controller_dc_init, controller_dc_read };