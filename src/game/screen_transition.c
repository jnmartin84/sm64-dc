#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "area.h"
#include "camera.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "game/game_init.h"
#include "geo_misc.h"
#include "gfx_dimensions.h"
#include "memory.h"
#include "screen_transition.h"
#include "segment2.h"
#include "sm64.h"

#include "sh4zam.h"

#define gSPRadarMark(pkt)          \
    {                              \
        Gfx* _g = (Gfx*) (pkt);    \
                                   \
        _g->words.w0 = 0x424C4E44; \
        _g->words.w1 = 0x87654321; \
    }

#define gSPCannon(pkt)          \
    {                              \
        Gfx* _g = (Gfx*) (pkt);    \
                                   \
        _g->words.w0 = 0x424C4E44; \
        _g->words.w1 = 0x87654322; \
    }


u8 sTransitionColorFadeCount[4] = { 0 };
u16 sTransitionTextureFadeCount[2] = { 0 };

s32 set_and_reset_transition_fade_timer(s8 fadeTimer, u8 transTime) {
    s32 reset = FALSE;

    sTransitionColorFadeCount[fadeTimer]++;

    if (sTransitionColorFadeCount[fadeTimer] == transTime) {
        sTransitionColorFadeCount[fadeTimer] = 0;
        sTransitionTextureFadeCount[fadeTimer] = 0;
        reset = TRUE;
    }
    return reset;
}

// note from jn64:
// if you do a bad job approximating the math, the fade-out transition has an opaque flash at the end
// just leave it as-is, not critical path
u8 set_transition_color_fade_alpha(s8 fadeType, s8 fadeTimer, u8 transTime) {
    u8 time = 0;

    switch (fadeType) {
//            time = (f32) sTransitionColorFadeCount[fadeTimer] * 255.0 / (f32)(transTime - 1) + 0.5; // fade in
//            break;
        case 1:
            time = (1.0 - sTransitionColorFadeCount[fadeTimer] / (f32)(transTime - 1)) * 255.0 + 0.5; // fade out
            break;
        case 0:
        default:
            time = (f32) sTransitionColorFadeCount[fadeTimer] * 255.0 / (f32)(transTime - 1) + 0.5; // fade in
            break;
    }
    return time;
}

Vtx *vertex_transition_color(struct WarpTransitionData *transData, u8 alpha) {
    Vtx *verts = alloc_display_list(4 * sizeof(*verts));
    u8 r = transData->red;
    u8 g = transData->green;
    u8 b = transData->blue;

    if (verts != NULL) {
        make_vertex(verts, 0, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 0, -1, 0, 0, r, g, b, alpha);
        make_vertex(verts, 1, GFX_DIMENSIONS_FROM_RIGHT_EDGE(0), 0, -1, 0, 0, r, g, b, alpha);
        make_vertex(verts, 2, GFX_DIMENSIONS_FROM_RIGHT_EDGE(0), SCREEN_HEIGHT, -1, 0, 0, r, g, b, alpha);
        make_vertex(verts, 3, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), SCREEN_HEIGHT, -1, 0, 0, r, g, b, alpha);
    } else {
    }
    return verts;
}

s32 dl_transition_color(s8 fadeTimer, u8 transTime, struct WarpTransitionData *transData, u8 alpha) {
    Vtx *verts = vertex_transition_color(transData, alpha);

    if (verts != NULL) {
        gSPRadarMark(gDisplayListHead++);
        gSPDisplayList(gDisplayListHead++, dl_proj_mtx_fullscreen);
        gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
        gSPDisplayList(gDisplayListHead++, dl_draw_quad_verts_0123);
        gSPRadarMark(gDisplayListHead++);
        gSPDisplayList(gDisplayListHead++, dl_screen_transition_end);
    }
    return set_and_reset_transition_fade_timer(fadeTimer, transTime);
}

s32 render_fade_transition_from_color(s8 fadeTimer, u8 transTime, struct WarpTransitionData *transData) {
    u8 alpha = set_transition_color_fade_alpha(1, fadeTimer, transTime);

    return dl_transition_color(fadeTimer, transTime, transData, alpha);
}

s32 render_fade_transition_into_color(s8 fadeTimer, u8 transTime, struct WarpTransitionData *transData) {
    u8 alpha = set_transition_color_fade_alpha(0, fadeTimer, transTime);

    return dl_transition_color(fadeTimer, transTime, transData, alpha);
}

s16 calc_tex_transition_radius(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData) {
    f32 texRadius = transData->endTexRadius - transData->startTexRadius;
    f32 radiusTime = shz_divf((sTransitionColorFadeCount[fadeTimer] * texRadius) , (f32)(transTime - 1));
    f32 result = transData->startTexRadius + radiusTime;

    return (s16)(result + 0.5f);
}

#include "sh4zam.h"

f32 calc_tex_transition_time(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData) {
    f32 startX = transData->startTexX;
    f32 startY = transData->startTexY;
    f32 endX = transData->endTexX;
    f32 endY = transData->endTexY;
    f32 sqrtfXY = shz_sqrtf_fsrra((startX - endX) * (startX - endX) + (startY - endY) * (startY - endY));
    f32 result = shz_divf((f32) sTransitionColorFadeCount[fadeTimer] * sqrtfXY , (f32)(transTime - 1));

    return result;
}

u16 convert_tex_transition_angle_to_pos(struct WarpTransitionData *transData) {
    f32 x = transData->endTexX - transData->startTexX;
    f32 y = transData->endTexY - transData->startTexY;

    return atan2s(x, y);
}

s16 center_tex_transition_x(struct WarpTransitionData *transData, f32 texTransTime, u16 texTransPos) {
    f32 x = transData->startTexX + coss(texTransPos) * texTransTime;

    return (s16)(x + 0.5f);
}

s16 center_tex_transition_y(struct WarpTransitionData *transData, f32 texTransTime, u16 texTransPos) {
    f32 y = transData->startTexY + sins(texTransPos) * texTransTime;

    return (s16)(y + 0.5f);
}

void make_tex_transition_vertex(Vtx *verts, s32 n, s8 fadeTimer, struct WarpTransitionData *transData, s16 centerTransX, s16 centerTransY,
                   s16 texRadius1, s16 texRadius2, s16 tx, s16 ty) {
    u8 r = transData->red;
    u8 g = transData->green;
    u8 b = transData->blue;
    u16 zeroTimer = sTransitionTextureFadeCount[fadeTimer];
    f32 centerX = texRadius1 * coss(zeroTimer) - texRadius2 * sins(zeroTimer) + centerTransX;
    f32 centerY = texRadius1 * sins(zeroTimer) + texRadius2 * coss(zeroTimer) + centerTransY;
    s16 x = round_float(centerX);
    s16 y = round_float(centerY);

    make_vertex(verts, n, x, y, -1, tx * 32, ty * 32, r, g, b, 255);
}

void load_tex_transition_vertex(Vtx *verts, s8 fadeTimer, struct WarpTransitionData *transData, s16 centerTransX, s16 centerTransY,
                   s16 texTransRadius, s8 transTexType) {
    switch (transTexType) {
        case TRANS_TYPE_MIRROR:
            make_tex_transition_vertex(verts, 0, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, -texTransRadius, -31, 63);
            make_tex_transition_vertex(verts, 1, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, -texTransRadius, 31, 63);
            make_tex_transition_vertex(verts, 2, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, texTransRadius, 31, 0);
            make_tex_transition_vertex(verts, 3, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, texTransRadius, -31, 0);
            break;
        case TRANS_TYPE_CLAMP:
            make_tex_transition_vertex(verts, 0, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, -texTransRadius, 0, 63);
            make_tex_transition_vertex(verts, 1, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, -texTransRadius, 63, 63);
            make_tex_transition_vertex(verts, 2, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, texTransRadius, 63, 0);
            make_tex_transition_vertex(verts, 3, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, texTransRadius, 0, 0);
            break;
    }
    make_tex_transition_vertex(verts, 4, fadeTimer, transData, centerTransX, centerTransY, -2000, -2000, 0, 0);
    make_tex_transition_vertex(verts, 5, fadeTimer, transData, centerTransX, centerTransY, 2000, -2000, 0, 0);
    make_tex_transition_vertex(verts, 6, fadeTimer, transData, centerTransX, centerTransY, 2000, 2000, 0, 0);
    make_tex_transition_vertex(verts, 7, fadeTimer, transData, centerTransX, centerTransY, -2000, 2000, 0, 0);
}

void *sTextureTransitionID[] = {
    texture_transition_star_half,
    texture_transition_circle_half,
    texture_transition_mario,
    texture_transition_bowser_half,
};

s32 render_textured_transition(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData, s8 texID, s8 transTexType) {
    f32 texTransTime = calc_tex_transition_time(fadeTimer, transTime, transData);
    u16 texTransPos = convert_tex_transition_angle_to_pos(transData);
    s16 centerTransX = center_tex_transition_x(transData, texTransTime, texTransPos);
    s16 centerTransY = center_tex_transition_y(transData, texTransTime, texTransPos);
    s16 texTransRadius = calc_tex_transition_radius(fadeTimer, transTime, transData);
    Vtx *verts = alloc_display_list(8 * sizeof(*verts));

    if (verts != NULL) {
        load_tex_transition_vertex(verts, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, transTexType);
        gSPDisplayList(gDisplayListHead++, dl_proj_mtx_fullscreen)
        gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
#ifdef GFX_BACKEND_PVR
        // raw-PVR: the border fill is the transition's only OPAQUE-list piece. As an opaque
        // full-screen overlay over live 3D it loses the PVR opaque depth-resolve to nearer world
        // geometry (z = 1/w, larger == nearer; close boss geometry beats the overlay's z2d ~1.0),
        // so the black frame vanishes on Bowser-death levels. The fade/center passes don't hit this
        // because they're XLU (TR list, composited last). The verts already carry alpha 255, so an
        // XLU draw is solid opaque black -- identical look, but now TR -> always on top. See the
        // GLdc path below for the stock OPA behaviour.
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
#else
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
#endif
        gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 8, 0);
        gSPRadarMark(gDisplayListHead++);
        gSPDisplayList(gDisplayListHead++, dl_transition_draw_filled_region);
        gDPPipeSync(gDisplayListHead++);
        gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        gDPSetTextureFilter(gDisplayListHead++, G_TF_BILERP);
        switch (transTexType) {
        case TRANS_TYPE_MIRROR:
            gDPLoadTextureBlock(gDisplayListHead++, sTextureTransitionID[texID], G_IM_FMT_IA, G_IM_SIZ_8b, 32, 64, 0,
                G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
            break;
        case TRANS_TYPE_CLAMP:
            gDPLoadTextureBlock(gDisplayListHead++, sTextureTransitionID[texID], G_IM_FMT_IA, G_IM_SIZ_8b, 64, 64, 0,
                G_TX_CLAMP, G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);
            break;
        }
        gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
        gSPDisplayList(gDisplayListHead++, dl_draw_quad_verts_0123);
        gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
        gSPRadarMark(gDisplayListHead++);
        gSPDisplayList(gDisplayListHead++, dl_screen_transition_end);
        sTransitionTextureFadeCount[fadeTimer] += transData->texTimer;
    } else {
    }
    return set_and_reset_transition_fade_timer(fadeTimer, transTime);
}

s32 render_screen_transition(s8 fadeTimer, s8 transType, u8 transTime, struct WarpTransitionData *transData) {
    switch (transType) {
        case WARP_TRANSITION_FADE_FROM_COLOR:
            return render_fade_transition_from_color(fadeTimer, transTime, transData);
            break;
        case WARP_TRANSITION_FADE_INTO_COLOR:
            return render_fade_transition_into_color(fadeTimer, transTime, transData);
            break;
        case WARP_TRANSITION_FADE_FROM_STAR:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_STAR, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_INTO_STAR:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_STAR, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_FROM_CIRCLE:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_CIRCLE, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_INTO_CIRCLE:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_CIRCLE, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_FROM_MARIO:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_MARIO, TRANS_TYPE_CLAMP);
            break;
        case WARP_TRANSITION_FADE_INTO_MARIO:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_MARIO, TRANS_TYPE_CLAMP);
            break;
        case WARP_TRANSITION_FADE_FROM_BOWSER:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_BOWSER, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_INTO_BOWSER:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_BOWSER, TRANS_TYPE_MIRROR);
            break;
    }
#ifdef AVOID_UB
    return 0;
#endif
}

Gfx *render_cannon_circle_base(void) {
    Vtx *verts = alloc_display_list(4 * sizeof(*verts));
    Gfx *dlist = alloc_display_list(22 * sizeof(*dlist));
    Gfx *g = dlist;

    if (verts != NULL && dlist != NULL) {
        make_vertex(verts, 0, 0, 0, -1, -1152, 1824, 0, 0, 0, 255);
        make_vertex(verts, 1, SCREEN_WIDTH, 0, -1, 1152, 1824, 0, 0, 0, 255);
        make_vertex(verts, 2, SCREEN_WIDTH, SCREEN_HEIGHT, -1, 1152, 192, 0, 0, 0, 255);
        make_vertex(verts, 3, 0, SCREEN_HEIGHT, -1, -1152, 192, 0, 0, 0, 255);

        gSPDisplayList(g++, dl_proj_mtx_fullscreen);
        gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
        gDPSetRenderMode(g++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        gDPSetTextureFilter(g++, G_TF_BILERP);
        gDPLoadTextureBlock(g++, sTextureTransitionID[TEX_TRANS_CIRCLE], G_IM_FMT_IA, G_IM_SIZ_8b, 32, 64, 0,
            G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
        gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        gSPVertex(g++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
        gSPCannon(g++);
        gSPDisplayList(g++, dl_draw_quad_verts_0123);
        gSPCannon(g++);
        gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
        gSPDisplayList(g++, dl_screen_transition_end);
        gSPEndDisplayList(g);
    } else {
        return NULL;
    }
    return dlist;
}

Gfx *geo_cannon_circle_base(s32 callContext, struct GraphNode *node, UNUSED Mat4 mtx) {
    struct GraphNodeGenerated *graphNode = (struct GraphNodeGenerated *) node;
    Gfx *dlist = NULL;

    if (callContext == GEO_CONTEXT_RENDER && gCurrentArea != NULL
        && gCurrentArea->camera->mode == CAMERA_MODE_INSIDE_CANNON) {
        graphNode->fnNode.node.flags = (graphNode->fnNode.node.flags & 0xFF) | 0x500;
        dlist = render_cannon_circle_base();
    }
    return dlist;
}
