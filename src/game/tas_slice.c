// TAS slices, for the debug menu's TAS replay and level select rows. A slice is the movie
// (tas/tas.m64) from the moment a level is entered, with the game state carried over from before:
// the save data, the random seed, the timers, Mario's lives and health, the act and the few globals
// that cross a level change. tas/tas_slices.bin holds one per level change of the 120-star run,
// recorded while the whole movie played, and first the game's start after the file select: that
// slice has no warp, so its level starts as a new game does, and the whole run begins with the
// intro (Lakitu, Peach's letter, the pipe). A replay sends the next level change to a slice's level
// instead, restores that state there and plays the movie on from the slice's sample, and when the
// level is left goes on to the next slice. Every level of the run is reached in seconds, and plays
// exactly as in the full run: the game logic is built without float shortcuts (Makefile), so it
// computes what the N64 does.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sm64.h"
#include "area.h"
#include "buffers/buffers.h"
#include "camera.h"
#include "game_init.h"
#include "ingame_menu.h"
#include "level_update.h"
#include "mario_misc.h"
#include "moving_texture.h"
#include "object_helpers.h"
#include "object_list_processor.h"
#include "paintings.h"
#include "rendering_graph_node.h"
#include "save_file.h"
#include "tas_slice.h"

#define WARP_TYPE_CHANGE_LEVEL 1   // level_update.c

extern s8 D_8032C9E0;
extern s8 sYoshiDead;
// camera.c, level_update.c, object_list_processor.c, behavior_actions.c globals without a header.
extern Vec3f sFixedModeBasePosition;
extern f32 gCameraZoomDist;
extern struct CameraStoredInfo sCameraStoreCutscene;
extern struct HandheldShakePoint sHandheldShakeSpline[4];
extern s16 sAvoidYawVel;
extern struct CameraFOVStatus sFOVState;
extern u32 gPrevLevel;
extern s32 sDelayedWarpArg;
extern s32 gEnvironmentLevels[20];
extern struct SpawnParticlesInfo D_8032F270;
extern s8 gDialogLineNum;
extern struct CutsceneVariable sCutsceneVars[10];
extern u32 gTimeStopState;
extern s16 gPaintingUpdateCounter, gLastPaintingUpdateCounter;
extern s16 sWasAtSurface, sSwimStrength, D_80339FD0, D_80339FD2;
extern f32 D_80339FD4;
extern struct ModeTransitionInfo sModeInfo;
extern struct TransitionInfo sModeTransition;
extern s16 sCameraYawAfterDoorCutscene, sCutsceneDialogID;
extern u8 sCutsceneDialogResponse;
extern s16 gSnowParticleMaxCount, gSnowParticleCount;
extern Vec3i gSnowCylinderLastPos;
extern s16 sSourceWarpNodeId;
extern f32 sObjSavedPosX, sObjSavedPosY, sObjSavedPosZ;
extern s8 gDialogCameraAngleIndex;
extern s32 gCurrLevelArea;
extern u8 gMenuHoldKeyIndex;
extern u8 gMenuHoldKeyTimer;
extern s16 sPrevCheckMarioRoom;
extern s8 gMainMenuDataModified;
extern u16 tas_random_seed16(void);
extern void tas_random_seed16_set(u16 seed);
extern unsigned int dc_tas_frame(void);
extern void dc_tas_seek(unsigned int sample);

// What crosses a level change, as plain bytes: no pointers.
#define ITEM(x) { &(x), sizeof(x), #x }
static const struct {
    void *addr;
    u32 size;
    const char *name;
} sItems[] = {
    ITEM(gGlobalTimer), ITEM(gAreaUpdateCounter), ITEM(gSaveBuffer), ITEM(gCurrSaveFileNum),
    ITEM(gSaveFileModified), ITEM(gWarpCheckpoint), ITEM(gCurrCourseNum), ITEM(gCurrActNum),
    ITEM(gSavedCourseNum), ITEM(gCurrCourseStarFlags), ITEM(gLastCompletedCourseNum),
    ITEM(gLastCompletedStarNum), ITEM(gSpecialTripleJump), ITEM(gHudDisplay), ITEM(sTimerRunning),
    ITEM(gShouldNotPlayCastleMusic), ITEM(D_8032C9E0), ITEM(sYoshiDead), ITEM(gTTCSpeedSetting),
    ITEM(gMarioCurrentRoom), ITEM(gPaintingMarioYEntry), ITEM(gPaintingMarioFloorType),
    ITEM(gPaintingMarioXPos), ITEM(gPaintingMarioYPos), ITEM(gPaintingMarioZPos), ITEM(gRedCoinsCollected),
    ITEM(gCameraMovementFlags), ITEM(sSelectionFlags), ITEM(sCButtonsPressed), ITEM(gLakituState),
    ITEM(sFixedModeBasePosition), ITEM(gCameraZoomDist), ITEM(sCameraStoreCutscene),
    ITEM(sHandheldShakeSpline), ITEM(sAvoidYawVel), ITEM(sFOVState), ITEM(gPrevLevel), ITEM(sDelayedWarpArg),
    ITEM(gEnvironmentLevels), ITEM(D_8032F270), ITEM(gDialogLineNum), ITEM(gLastDialogLineNum),
    ITEM(gDialogTextAlpha), ITEM(gDialogColorFadeTimer), ITEM(gControllerPads[0]), ITEM(gMarioStates[0].slideYaw),
    ITEM(sCutsceneVars), ITEM(gTimeStopState), ITEM(gPaintingUpdateCounter), ITEM(gLastPaintingUpdateCounter),
    ITEM(sWasAtSurface), ITEM(sSwimStrength), ITEM(D_80339FD0), ITEM(D_80339FD2), ITEM(D_80339FD4),
    ITEM(sModeInfo), ITEM(sModeTransition), ITEM(sCameraYawAfterDoorCutscene), ITEM(sCutsceneDialogID),
    ITEM(sCutsceneDialogResponse), ITEM(gSnowParticleMaxCount), ITEM(gSnowParticleCount),
    ITEM(gSnowCylinderLastPos), ITEM(sSourceWarpNodeId), ITEM(gBodyStates[0]), ITEM(sObjSavedPosX),
    ITEM(sObjSavedPosY), ITEM(sObjSavedPosZ), ITEM(gDialogCameraAngleIndex), ITEM(gCurrLevelArea),
    ITEM(gMarioStates[0].twirlYaw), ITEM(gMarioStates[0].fadeWarpOpacity),
    // The pause menu's key repeat: the cursor moves by its timer, which the last menu left.
    ITEM(gMenuHoldKeyIndex), ITEM(gMenuHoldKeyTimer), ITEM(sPrevCheckMarioRoom),
    // A coin score or sound mode change not yet saved: the save writes the menu data's backup.
    ITEM(gMainMenuDataModified),
    // Mario's action too: coming from the file select he has none (ACT_UNINITIALIZED), and a
    // warp then spawns no Mario (init_mario_after_warp).
    ITEM(gMarioStates[0].action), ITEM(gMarioStates[0].numLives), ITEM(gMarioStates[0].health), ITEM(gMarioStates[0].numStars),
    ITEM(gMarioStates[0].numCoins), ITEM(gMarioStates[0].numKeys), ITEM(gMarioStates[0].flags),
    ITEM(gMarioStates[0].capTimer), ITEM(gMarioStates[0].prevNumStarsForDialog),
    ITEM(gMarioStates[0].unkB0), ITEM(gMarioStates[0].hurtCounter), ITEM(gMarioStates[0].healCounter),
    // init_mario leaves these as the last level had them: a slide's velocity, say, which the next
    // level's first slide or water exit picks up (the moat door's hyperspeed after VCUTM).
    ITEM(gMarioStates[0].slideVelX), ITEM(gMarioStates[0].slideVelZ), ITEM(gMarioStates[0].prevAction),
    ITEM(gMarioStates[0].actionState), ITEM(gMarioStates[0].actionArg), ITEM(gMarioStates[0].peakHeight),
    ITEM(gMarioStates[0].wallKickTimer), ITEM(gMarioStates[0].doubleJumpTimer), ITEM(gMarioStates[0].unkC4),
    ITEM(gControllers[0].rawStickX), ITEM(gControllers[0].rawStickY), ITEM(gControllers[0].stickX),
    ITEM(gControllers[0].stickY), ITEM(gControllers[0].stickMag), ITEM(gControllers[0].buttonDown),
    ITEM(gControllers[0].buttonPressed),
};

// A slice in the file: its header, then the items in sItems order.
struct SliceHeader {
    u32 sample;   // the movie's next sample at init_level
    struct WarpDest warp;
    u16 seed;
    u16 itemBytes;
};

static u8 *sSlices;       // the slices file
static u32 sSliceCount, sSliceBytes;
static s32 sPlaylist[256];
static u32 sPlayCount, sPlayNext;
static s32 sPlayOn;                   // the game goes on after the last slice
static struct SliceHeader *sPending;   // set by a redirected warp, restored at init_level
static s32 sPlaying = -1;             // the slice being played
// The slice that just ended, and where its level change went before the redirect: the full movie
// went there too, and its next slice holds the state it had then.
static s32 sEnded = -1;
static struct WarpDest sEndedWarp;
static s16 sEndedAct;
static s32 sEndedActSelect;           // the movie went through the act selector, the replay doesn't
static u32 sSlicesExact;
static void tas_menu_build(void);

static u32 item_bytes(void) {
    u32 n = 0;
    for (u32 i = 0; i < ARRAY_COUNT(sItems); i++) {
        n += sItems[i].size;
    }
    return n;
}

// A slice's bytes in the file, padded so the next header is 4-byte aligned (the SH4 faults on a
// misaligned load).
static u32 slice_bytes(void) {
    return (sizeof(struct SliceHeader) + item_bytes() + 3) & ~3u;
}

static struct SliceHeader *slice_at(u32 index) {
    return (struct SliceHeader *) (sSlices + 8 + index * sSliceBytes);
}

// Reads sliceFile ("SLS1", the slice size, then the slices); the debug menu picks what plays.
void tas_slice_setup(const char *sliceFile) {
    FILE *f = fopen(sliceFile, "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sSlices = malloc((size_t) size);
    // In 32 KB reads: one read of the whole file over dcload's /pc stalled the console.
    long got = 0;
    while (sSlices != NULL && got < size) {
        size_t n = fread(sSlices + got, 1, size - got < 32768 ? (size_t) (size - got) : 32768, f);
        if (n == 0) {
            break;
        }
        got += (long) n;
    }
    if (sSlices == NULL || size < 8 || got != size
        || ((u32 *) sSlices)[0] != 0x31534C53
        || ((u32 *) sSlices)[1] != slice_bytes()) {
        printf("TAS: %s is not a slices file of this build\n", sliceFile);
        free(sSlices);
        sSlices = NULL;
        fclose(f);
        return;
    }
    fclose(f);
    sSliceBytes = ((u32 *) sSlices)[1];
    sSliceCount = (u32) (size - 8) / sSliceBytes;
    printf("TAS: %u slices\n", (unsigned) sSliceCount);
    tas_menu_build();
}

// One item's bytes in a slice.
static const u8 *slice_item(const struct SliceHeader *s, const void *addr) {
    const u8 *p = (const u8 *) (s + 1);
    for (u32 i = 0; i < ARRAY_COUNT(sItems) && sItems[i].addr != addr; i++) {
        p += sItems[i].size;
    }
    return p;
}

static struct SliceHeader *next_slice(void) {
    if (sSlices == NULL || sPlayNext >= sPlayCount) {
        return NULL;
    }
    sPlaying = sPlaylist[sPlayNext++];
    struct SliceHeader *s = slice_at((u32) sPlaying);
    // The act now: the level script places the act's objects while it loads the level, before
    // init_level restores the rest.
    memcpy(&gCurrActNum, slice_item(s, &gCurrActNum), sizeof(gCurrActNum));
    printf("TAS: slice level %d area %d from sample %u\n", s->warp.levelNum, s->warp.areaIdx,
           (unsigned) s->sample);
    return s;
}

// lvl_init_from_save_file: the game's first level, or the first slice's.
s32 tas_slice_first_level(s32 levelNum) {
    if (sSlices != NULL && sPlayNext == 0 && (sPending = next_slice()) != NULL) {
        D_8032C9E0 = 1;   // no act selector: the slice has the act
        return sPending->warp.levelNum;
    }
    return levelNum;
}

// The playlist is done.
int gTasSlicesDone;
// A replay from the debug menu is done: the movie stops at the next level's init_level, after its
// end is checked, and the controller plays on from there.
static s32 sStopAtInit;
static s32 sMovie = 1;   // 0: the level select's slice, played by the controller from its state
extern void dc_tas_stop(void);

// initiate_warp: once a slice's level is left, the next level change goes to the next slice.
void tas_slice_warp(s16 *destLevel, s16 *destArea, s16 *destWarpNode, s32 *arg) {
    // A replay the menu could not start in the level (tas_menu_level_change): from this level change.
    if (sSlices != NULL && sPlayNext == 0 && sPlayCount > 0 && *destLevel != gCurrLevelNum
        && (sPending = next_slice()) != NULL) {
        *destLevel = sPending->warp.levelNum;
        *destArea = sPending->warp.areaIdx;
        *destWarpNode = sPending->warp.nodeId;
        *arg = (s32) sPending->warp.arg;
        D_8032C9E0 = 1;
        return;
    }
    if (sSlices == NULL || sPlayNext == 0 || *destLevel == gCurrLevelNum) {
        return;
    }
    if (sPlayNext >= sPlayCount && !gTasSlicesDone) {
        gTasSlicesDone = 1;
        printf("TAS: slices done\n");
        sStopAtInit = !sPlayOn;
    }
    sEnded = sPlaying;
    sEndedWarp.levelNum = (u8) *destLevel;
    sEndedWarp.areaIdx = (u8) *destArea;
    sEndedWarp.nodeId = (u8) *destWarpNode;
    sEndedWarp.arg = (u32) *arg;
    sEndedAct = gCurrActNum;
    s32 course = gLevelToCourseNumTable[*destLevel - 1];   // as lvl_set_current_level decides
    sEndedActSelect = course != COURSE_NONE && course <= COURSE_STAGES_MAX && D_8032C9E0 == 0;
    if ((sPending = next_slice()) != NULL) {
        // A warp within the level never reaches init_level, so the slice's state isn't restored.
        if (sPending->warp.levelNum == gCurrLevelNum) {
            printf("TAS: slice %d is in the level before it: it cannot play\n", (int) sPlaying);
        }
        *destLevel = sPending->warp.levelNum;
        *destArea = sPending->warp.areaIdx;
        *destWarpNode = sPending->warp.nodeId;
        *arg = (s32) sPending->warp.arg;
        D_8032C9E0 = 1;   // no act selector: the slice has the act
    }
}

// A slice that ended exactly as in the full movie arrives where the movie's next slice starts, at
// its sample, with its seed and state. Reports the first differences, so a replay that drifted (a
// death the movie doesn't have, say) shows in the log rather than being hidden by the next slice.
static void slice_check_end(void) {
    if (sEnded < 0 || (u32) sEnded + 1 >= sSliceCount) {
        return;
    }
    const struct SliceHeader *e = slice_at((u32) sEnded + 1);
    u32 bad = 0;
    // The act selector's frames are skipped, and with them the time and the pad that passed there,
    // its menu's key repeat and the act chosen.
    static const void *const selector[] = {
        &gGlobalTimer, &gAreaUpdateCounter, &gControllerPads[0], &gCurrActNum, &gMenuHoldKeyIndex,
        &gMenuHoldKeyTimer, &gControllers[0].rawStickX, &gControllers[0].rawStickY,
        &gControllers[0].stickX, &gControllers[0].stickY, &gControllers[0].stickMag,
        &gControllers[0].buttonDown, &gControllers[0].buttonPressed,
    };
    if ((dc_tas_frame() != e->sample && !sEndedActSelect) || sEndedWarp.levelNum != e->warp.levelNum
        || sEndedWarp.areaIdx != e->warp.areaIdx || sEndedWarp.nodeId != e->warp.nodeId) {
        printf("TAS: slice %d ended at sample %u level %d area %d node %d, the movie at %u level %d area %d node %d\n",
               (int) sEnded, (unsigned) dc_tas_frame(), sEndedWarp.levelNum, sEndedWarp.areaIdx,
               sEndedWarp.nodeId, (unsigned) e->sample, e->warp.levelNum, e->warp.areaIdx, e->warp.nodeId);
        bad++;
    }
    if (tas_random_seed16() != e->seed) {
        printf("TAS: slice %d ended with seed %04x, the movie %04x\n", (int) sEnded,
               (unsigned) tas_random_seed16(), (unsigned) e->seed);
        bad++;
    }
    // Loading the playlist's next level, not the movie's, has already set these.
    static const void *const loaded[] = {
        &gCurrCourseNum, &gSavedCourseNum, &gCurrCourseStarFlags, &gHudDisplay,
        &gPaintingUpdateCounter, &gLastPaintingUpdateCounter, &gMarioStates[0].numCoins,
        &gWarpCheckpoint,
    };
    const u8 *p = (const u8 *) (e + 1);
    for (u32 i = 0; i < ARRAY_COUNT(sItems); i++) {
        u32 k = 0;
        while (k < ARRAY_COUNT(loaded) && loaded[k] != sItems[i].addr) {
            k++;
        }
        u32 j = 0;
        while (sEndedActSelect && j < ARRAY_COUNT(selector) && selector[j] != sItems[i].addr) {
            j++;
        }
        if (k < ARRAY_COUNT(loaded) || (sEndedActSelect && j < ARRAY_COUNT(selector))) {
            p += sItems[i].size;
            continue;
        }
        const u8 *now = sItems[i].addr == &gCurrActNum ? (const u8 *) &sEndedAct : sItems[i].addr;
        if (memcmp(now, p, sItems[i].size) != 0 && bad++ < 8) {
            u32 at = 0;
            while (now[at] == p[at]) {
                at++;
            }
            printf("TAS: slice %d ended with %s differing from byte %u\n", (int) sEnded, sItems[i].name,
                   (unsigned) at);
        }
        p += sItems[i].size;
    }
    if (bad == 0) {
        sSlicesExact++;
    }
    printf("TAS: slice %d ended %s%s (%u of the checked exact)\n", (int) sEnded,
           bad ? "DIFFERENT from the movie" : "as in the movie",
           sEndedActSelect ? ", up to its act selector" : "", (unsigned) sSlicesExact);
    sEnded = -1;
}

// init_level, before the level's warp is carried out.
void tas_slice_init_level(void) {
    if (sStopAtInit) {   // the replay's last slice ended: checked, and the controller takes over
        if (sMovie) {
            slice_check_end();
        }
        sEnded = -1;
        dc_tas_stop();
        sStopAtInit = 0;
        sPlayCount = sPlayNext = 0;
        sPlaying = -1;
    }
    if (sPending != NULL) {
        slice_check_end();
        const u8 *p = (const u8 *) (sPending + 1);
        for (u32 i = 0; i < ARRAY_COUNT(sItems); i++) {
            memcpy(sItems[i].addr, p, sItems[i].size);
            p += sItems[i].size;
        }
        sWarpDest = sPending->warp;   // the start slice's: no warp, so the level starts as a new game
        tas_random_seed16_set(sPending->seed);
        if (!sMovie) {
            dc_tas_stop();   // the level select's: the controller plays from here
        } else
        dc_tas_seek(sPending->sample);
        sPending = NULL;
    }
}

// make DC_TAS_MENU=1: the movie in the debug menu. A section is a slice that collects a star or a
// few, or a Bowser fight; what it collects is the difference between its save data and the next
// slice's. A replay plays over the player's game from the next level change (at once in a level),
// and gives the controller back where it ends. From the first replay on, saves stay in memory, so
// the movie's save never reaches the VMU.
#include "course_table.h"
#include "level_table.h"
#include "memory.h"
#include "segment2.h"

#define TAS_SECTIONS 160
#define SECTION_STARS 4
static void tas_level_build(void);

struct TasSection {
    u16 slice;
    u8 course;                      // where its stars are: COURSE_NONE for the castle's
    u8 count;                       // stars in star[]
    u8 star[SECTION_STARS][2];      // course, star index (0-6; the castle's: 0-2 Toad, 3-4 MIPS)
    s16 before;                     // Mario's stars when it starts
    s16 after;
    char label[24];
};

static struct TasSection sSections[TAS_SECTIONS];
static const char *sMenuLabels[TAS_SECTIONS + 2];
static s32 sSectionCount;
static char sHelp[320];
extern s32 gTasSaveInMemory;
extern unsigned int dc_tas_samples(void);
extern s32 tas_menu_level_change(const struct WarpDest *dest);   // level_update.c

static const char *const sCourseShort[] = {
    "Castle", "BoB", "WF", "JRB", "CCM", "BBH", "HMC", "LLL", "SSL", "DDD", "SL", "WDW", "TTM",
    "THI", "TTC", "RR", "BitDW", "BitFS", "BitS", "PSS", "CotMC", "TotWC", "VCutM", "WMotR", "SA",
    "End",
};

static void slice_save(const struct SliceHeader *s, struct SaveFile *out) {
    struct SaveBuffer save;
    s16 file;
    memcpy(&save, slice_item(s, &gSaveBuffer), sizeof(save));
    memcpy(&file, slice_item(s, &gCurrSaveFileNum), sizeof(file));
    *out = save.files[file >= 1 && file <= NUM_SAVE_FILES ? file - 1 : 0][0];
}

static s16 slice_stars(const struct SliceHeader *s) {
    s16 stars;
    memcpy(&stars, slice_item(s, &gMarioStates[0].numStars), sizeof(stars));
    return stars;
}

// The movie's time at a sample: the game reads the controller once a frame, 30 a second.
static u32 sample_seconds(u32 sample) {
    return sample / 30;
}

static void section_add_star(struct TasSection *t, u8 course, u8 star) {
    if (t->count < SECTION_STARS) {
        t->star[t->count][0] = course;
        t->star[t->count][1] = star;
    }
    t->count++;
}

static void tas_menu_build(void) {
    sMenuLabels[0] = "Off";
    sMenuLabels[1] = "All 120 stars";
    sSectionCount = 0;
    for (u32 i = 0; i < sSliceCount && sSectionCount < TAS_SECTIONS; i++) {
        const struct SliceHeader *s = slice_at(i);
        const struct SliceHeader *e = i + 1 < sSliceCount ? slice_at(i + 1) : NULL;
        struct SaveFile was, now;
        slice_save(s, &was);
        if (e != NULL) {
            slice_save(e, &now);
        } else {
            now = was;
        }
        struct TasSection *t = &sSections[sSectionCount];
        memset(t, 0, sizeof(*t));
        t->slice = (u16) i;
        t->course = (u8) gLevelToCourseNumTable[s->warp.levelNum - 1];
        t->before = slice_stars(s);
        t->after = e != NULL ? slice_stars(e) : t->before;
        for (u8 c = 0; c < COURSE_COUNT; c++) {
            u8 got = now.courseStars[c] & ~was.courseStars[c] & 0x7F;
            for (u8 b = 0; b < 7; b++) {
                if (got >> b & 1) section_add_star(t, (u8) (c + 1), b);
            }
        }
        u8 castle = (u8) ((now.flags & ~was.flags) >> 24 & 0x7F);
        for (u8 b = 0; b < 7; b++) {
            if (castle >> b & 1) section_add_star(t, COURSE_NONE, b);
        }
        s32 level = s->warp.levelNum;
        s32 bowser = level == LEVEL_BOWSER_1 ? 1 : level == LEVEL_BOWSER_2 ? 2 : level == LEVEL_BOWSER_3 ? 3 : 0;
        if (t->count == 0 && !bowser) {
            continue;
        }
        // "BoB 1", "WF 3+4", "SSL 100", "Toad 2", "Bowser 1".
        char *l = t->label;
        size_t room = sizeof(t->label);
        if (bowser) {
            snprintf(l, room, "Bowser %d", (int) bowser);
        } else {
            u8 course = t->star[0][0];
            int n = snprintf(l, room, "%s ", course == COURSE_NONE ? (t->star[0][1] < 3 ? "Toad" : "MIPS")
                                                                   : sCourseShort[course]);
            for (u8 k = 0; k < t->count && k < SECTION_STARS && n > 0 && (size_t) n < room; k++) {
                u8 star = t->star[k][1];
                if (course == COURSE_NONE) {
                    star = star < 3 ? star : (u8) (star - 3);
                }
                if (star == 6) {
                    n += snprintf(l + n, room - (size_t) n, k ? "+100" : "100");
                } else {
                    n += snprintf(l + n, room - (size_t) n, k ? "+%d" : "%d", star + 1);
                }
            }
        }
        sMenuLabels[2 + sSectionCount] = t->label;
        sSectionCount++;
    }
    tas_level_build();
}

s32 tas_menu_count(void) {
    return sSlices != NULL ? 2 + sSectionCount : 0;
}

const char *const *tas_menu_labels(void) {
    return sMenuLabels;
}

// The game's own text (the courses' and the acts' names, in its character set) as ASCII, in title
// case: "BIG BOB-OMB ON THE SUMMIT" -> "Big Bob-omb on the Summit".
static size_t game_text(char *out, size_t room, const u8 *text) {
    static const char *const small[] = { "a", "an", "and", "at", "by", "for", "in", "into", "of",
                                         "on", "onto", "the", "to", "with" };
    size_t n = 0;
    while (*text == 0x9E || *text <= 0x09) {   // the course number before its name
        text++;
    }
    size_t word = 0;
    for (; *text != 0xFF && n + 1 < room; text++) {
        u8 c = *text;
        char ch = c <= 0x09 ? (char) ('0' + c) : c <= 0x23 ? (char) ('A' + c - 0x0A)
                : c <= 0x3D ? (char) ('a' + c - 0x24) : c == 0x3E ? '\'' : c == 0x3F ? '.'
                : c == 0x6F ? ',' : c == 0x9E ? ' ' : c == 0x9F ? '-' : c == 0xE5 ? '&'
                : c == 0xE6 ? ':' : c == 0xF2 ? '!' : c == 0xF4 ? '?' : 0;
        if (!ch) {
            continue;
        }
        int first = n == word;
        if (ch >= 'A' && ch <= 'Z' && !first) {
            ch = (char) (ch - 'A' + 'a');
        }
        out[n++] = ch;
        if (ch == ' ' || (ch == '-' && !(text[1] == 0x18 && text[2] == 0x16 && text[3] == 0x0B))) {
            word = n;   // a new word, but "Bob-omb"
        }
    }
    out[n] = 0;
    // Small words in lower case, but the first.
    for (size_t k = 1; k < n; k++) {
        if (out[k - 1] != ' ') continue;
        size_t end = k;
        while (end < n && out[end] != ' ') end++;
        for (u32 w = 0; w < ARRAY_COUNT(small); w++) {
            size_t len = strlen(small[w]);
            if (len == end - k && strncasecmp(out + k, small[w], len) == 0) {
                out[k] = (char) (out[k] | 0x20);
            }
        }
    }
    return n;
}

static size_t star_name(char *out, size_t room, u8 course, u8 star) {
    if (course == COURSE_NONE) {
        return (size_t) snprintf(out, room, star < 3 ? "Toad's star %d" : "MIPS %d", star < 3 ? star + 1 : star - 2);
    }
    if (COURSE_IS_MAIN_COURSE(course) && star < 6) {
        void **acts = segmented_to_virtual(seg2_act_name_table);
        return game_text(out, room, segmented_to_virtual(acts[(course - 1) * 6 + star]));
    }
    if (COURSE_IS_MAIN_COURSE(course)) {
        return (size_t) snprintf(out, room, "100 coins");
    }
    if (course == COURSE_PSS) {
        return (size_t) snprintf(out, room, star == 0 ? "the slide" : "under 21 seconds");
    }
    return (size_t) snprintf(out, room, "8 red coins");
}

static size_t course_name(char *out, size_t room, u8 course) {
    if (course == COURSE_NONE) {
        return (size_t) snprintf(out, room, "The castle");
    }
    void **courses = segmented_to_virtual(seg2_course_name_table);
    return game_text(out, room, segmented_to_virtual(courses[course - 1]));
}

const char *tas_menu_help(s32 entry) {
    u32 total = sample_seconds(dc_tas_samples());
    if (entry <= 0 || entry >= tas_menu_count()) {
        snprintf(sHelp, sizeof(sHelp),
                 "The 120-star TAS, a star or two or the whole run, over your game from the next "
                 "level change, with the TAS's save (the VMU's is left alone). Off and A: the pad "
                 "plays again, from your VMU save at the title.");
        return sHelp;
    }
    if (entry == 1) {
        snprintf(sHelp, sizeof(sHelp),
                 "The whole 120-star run, from the castle grounds to the credits: %d sections, "
                 "%u:%02u:%02u. A: play it.",
                 (int) sSectionCount, (unsigned) (total / 3600), (unsigned) (total / 60 % 60),
                 (unsigned) (total % 60));
        return sHelp;
    }
    const struct TasSection *t = &sSections[entry - 2];
    const struct SliceHeader *s = slice_at(t->slice);
    u32 end = t->slice + 1u < sSliceCount ? slice_at(t->slice + 1u)->sample : dc_tas_samples();
    u32 length = sample_seconds(end - s->sample), at = sample_seconds(s->sample);
    s32 level = s->warp.levelNum;
    u8 course = t->count ? t->star[0][0] : (u8) gLevelToCourseNumTable[level - 1];
    size_t n = course_name(sHelp, sizeof(sHelp), course);
    if (level == LEVEL_BOWSER_1 || level == LEVEL_BOWSER_2 || level == LEVEL_BOWSER_3) {
        n += (size_t) snprintf(sHelp + n, sizeof(sHelp) - n, level == LEVEL_BOWSER_3 ? ": the last fight"
                                                                                     : ": the fight for the key");
    }
    for (u8 k = 0; k < t->count && k < SECTION_STARS && n + 4 < sizeof(sHelp); k++) {
        n += (size_t) snprintf(sHelp + n, sizeof(sHelp) - n, k ? ", " : ": ");
        n += star_name(sHelp + n, sizeof(sHelp) - n, t->star[k][0], t->star[k][1]);
    }
    if (n + 1 < sizeof(sHelp)) {
        snprintf(sHelp + n, sizeof(sHelp) - n,
                 "\n%d star%s, %d to %d of 120. %u:%02u long, at %u:%02u:%02u of the run.\nSection %u "
                 "of the movie. A: play it.",
                 (int) t->count, t->count == 1 ? "" : "s", (int) t->before, (int) t->after,
                 (unsigned) (length / 60), (unsigned) (length % 60), (unsigned) (at / 3600),
                 (unsigned) (at / 60 % 60), (unsigned) (at % 60), (unsigned) t->slice);
    }
    return sHelp;
}

static void menu_stop(void) {
    sPending = NULL;
    sPlayCount = sPlayNext = 0;
    sPlaying = sEnded = -1;
    sStopAtInit = 0;
    gTasSlicesDone = 0;
    sMovie = 1;
    dc_tas_stop();
}

// Plays the playlist from the next level change: in a level at once (tas_menu_level_change), from
// the title through the file select's first level. movie 0: the controller plays the slice.
static void menu_play(s32 movie, const char *what) {
    sMovie = movie;
    gTasSaveInMemory = 1;
    printf("TAS: %s of %s armed\n", movie ? "replay" : "level", what);
    // In a level now: its warp goes to the slice's level, whose init_level restores the rest.
    struct SliceHeader *s = slice_at((u32) sPlaylist[0]);
    if (tas_menu_level_change(&s->warp)) {
        sPending = next_slice();
        D_8032C9E0 = 1;   // no act selector: the slice has the act
    }
}

// Entry 0 gives the game back to the controller, 1 plays the whole run, the others a section.
extern void tas_menu_reset_game(void);   // level_update.c
// Off: the pad plays again. After a replay or a level select the save was the movie's: the
// player's comes back from the VMU, and in a level the game goes back to the title.
static void menu_off(void) {
    menu_stop();
    printf("TAS: the controller plays\n");
    if (gTasSaveInMemory) {
        gTasSaveInMemory = 0;
        save_file_load_all();
        tas_menu_reset_game();
        printf("TAS: the save is the VMU's again\n");
    }
}
void tas_menu_start(s32 entry) {
    if (entry <= 0 || entry >= tas_menu_count()) {
        menu_off();
        return;
    }
    menu_stop();
    if (entry == 1) {
        for (u32 i = 0; i < sSliceCount && sPlayCount < ARRAY_COUNT(sPlaylist); i++) {
            sPlaylist[sPlayCount++] = (s32) i;
        }
        sPlayOn = 1;
    } else {
        sPlaylist[sPlayCount++] = sSections[entry - 2].slice;
        sPlayOn = 0;
    }
    menu_play(1, sMenuLabels[entry]);
}

// The level select: each of the 120 stars (and the Bowser fights) by course, played by the
// controller from the state the movie had when it entered that level for it.
#define TAS_LEVELS (120 + 3 + 8)

struct TasLevel {
    u16 section;
    u8 course, star;   // as in struct TasSection; star 7 + n for Bowser n
};

static struct TasLevel sLevels[TAS_LEVELS];
static char sLevelText[TAS_LEVELS][16];
static const char *sLevelLabels[TAS_LEVELS + 1];
static s32 sLevelCount;

// Course order, then star: the castle's stars after the courses, the Bowser fights last.
static u32 level_key(const struct TasLevel *l) {
    return (l->star >= 8 ? 40u : l->course == COURSE_NONE ? 30u : l->course) * 16u + l->star;
}

static void level_add(u16 section, u8 course, u8 star) {
    if (sLevelCount >= TAS_LEVELS) {
        return;
    }
    struct TasLevel l = { section, course, star };
    s32 at = sLevelCount++;
    while (at > 0 && level_key(&sLevels[at - 1]) > level_key(&l)) {
        sLevels[at] = sLevels[at - 1];
        at--;
    }
    sLevels[at] = l;
}

static void tas_level_build(void) {
    sLevelCount = 0;
    for (s32 i = 0; i < sSectionCount; i++) {
        const struct TasSection *t = &sSections[i];
        for (u8 k = 0; k < t->count && k < SECTION_STARS; k++) {
            level_add((u16) i, t->star[k][0], t->star[k][1]);
        }
        s32 level = slice_at(t->slice)->warp.levelNum;
        if (level == LEVEL_BOWSER_1 || level == LEVEL_BOWSER_2 || level == LEVEL_BOWSER_3) {
            level_add((u16) i, (u8) gLevelToCourseNumTable[level - 1],
                      (u8) (7 + (level == LEVEL_BOWSER_1 ? 1 : level == LEVEL_BOWSER_2 ? 2 : 3)));
        }
    }
    sLevelLabels[0] = "Off";
    for (s32 i = 0; i < sLevelCount; i++) {
        const struct TasLevel *l = &sLevels[i];
        u8 star = l->star;
        if (star >= 8) {
            snprintf(sLevelText[i], sizeof(sLevelText[i]), "Bowser %d", star - 7);
        } else if (l->course == COURSE_NONE) {
            snprintf(sLevelText[i], sizeof(sLevelText[i]), star < 3 ? "Toad %d" : "MIPS %d", star < 3 ? star + 1 : star - 2);
        } else if (star == 6) {
            snprintf(sLevelText[i], sizeof(sLevelText[i]), "%s 100", sCourseShort[l->course]);
        } else {
            snprintf(sLevelText[i], sizeof(sLevelText[i]), "%s %d", sCourseShort[l->course], star + 1);
        }
        sLevelLabels[1 + i] = sLevelText[i];
    }
}

s32 tas_level_count(void) {
    return sSlices != NULL ? 1 + sLevelCount : 0;
}

const char *const *tas_level_labels(void) {
    return sLevelLabels;
}

const char *tas_level_help(s32 entry) {
    if (entry <= 0 || entry >= tas_level_count()) {
        snprintf(sHelp, sizeof(sHelp),
                 "Each star, and the Bowser fights, to play yourself: from the save and the moment the "
                 "TAS had when it entered that level for it (the VMU's save is left alone). Off and A: "
                 "your VMU save again, from the title.");
        return sHelp;
    }
    const struct TasLevel *l = &sLevels[entry - 1];
    const struct TasSection *t = &sSections[l->section];
    const struct SliceHeader *s = slice_at(t->slice);
    s16 act;
    memcpy(&act, slice_item(s, &gCurrActNum), sizeof(act));
    u32 at = sample_seconds(s->sample);
    size_t n = course_name(sHelp, sizeof(sHelp), l->course);
    if (l->star >= 8) {
        n += (size_t) snprintf(sHelp + n, sizeof(sHelp) - n, l->star == 10 ? ": the last fight" : ": the fight for the key");
    } else {
        n += (size_t) snprintf(sHelp + n, sizeof(sHelp) - n, ": ");
        n += star_name(sHelp + n, sizeof(sHelp) - n, l->course, l->star);
    }
    if (COURSE_IS_MAIN_COURSE(l->course) && n + 1 < sizeof(sHelp)) {
        n += (size_t) snprintf(sHelp + n, sizeof(sHelp) - n, "\nAct %d, with", (int) act);
    } else if (n + 1 < sizeof(sHelp)) {
        n += (size_t) snprintf(sHelp + n, sizeof(sHelp) - n, "\nWith");
    }
    if (n + 1 < sizeof(sHelp)) {
        snprintf(sHelp + n, sizeof(sHelp) - n, " %d of 120 stars, as the TAS came there at %u:%02u:%02u. A: play it.",
                 (int) t->before, (unsigned) (at / 3600), (unsigned) (at / 60 % 60), (unsigned) (at % 60));
    }
    return sHelp;
}

// Entry 0 is Off, as in the TAS replay row.
void tas_level_start(s32 entry) {
    if (entry <= 0) {
        menu_off();
        return;
    }
    if (entry >= tas_level_count()) {
        return;
    }
    menu_stop();
    sPlaylist[sPlayCount++] = sSections[sLevels[entry - 1].section].slice;
    sPlayOn = 0;
    menu_play(0, sLevelLabels[entry]);
}

