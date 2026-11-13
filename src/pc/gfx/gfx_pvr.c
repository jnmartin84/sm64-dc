// gfx_pvr.c — raw-KOS-PVR backend for the SM64 DC renderer (ported from MK64).
//
// Drop-in replacement for the GLdc backend (gfx_gldc.c): implements the same
// struct GfxRenderingAPI, but submits geometry straight to the PVR via the
// direct-render (store-queue) path instead of going through OpenGL. The
// gfx_retro_dc.c front-end (display-list commands, combiner, SW clip/cull/scissor)
// is shared; only this backend differs. SM64's rapi is the SINGLE-TEXTURE variant
// (shader_get_info returns the used-texture bool; no tile params), so this backend
// is adapted accordingly vs MK64's two-tile version.

#include <PR/gbi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
//#include <stdbool.h>
#undef bool
#define bool uint8_t
#include <assert.h>
#include <kos.h>
#include <dc/video.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"
#include "gl_fast_vert.h"   // dc_fast_t (the front-end's per-vertex emit struct)
#include "sh4zam_port.h"
#include <enDjinn/enj_state.h>

extern int cur_frame_persp;
extern int prev_frame_had_persp;
extern int has_done_3d;

// The DR submit path relies on these: each vertex is copied dc_fast_t -> pvr_vertex_t
// (identical 32-byte layout), and the poly header is written into a single 32-byte
// store-queue slot. If a KOS revision changes either size, fail at build time rather
// than corrupting the SQ at runtime.
_Static_assert(sizeof(dc_fast_t) == sizeof(pvr_vertex_t), "dc_fast_t must match pvr_vertex_t layout");
_Static_assert(sizeof(pvr_poly_hdr_t) == 32, "pvr_poly_hdr_t must be one 32-byte SQ slot");

// FSAA: the PVR renders twice as wide and the scaler filters down on output, so every x the
// front-end bakes is in a (DC_FB_W << 1)-wide space (gfx_retro_dc.c DC_FSAA_X).
// The framebuffer: 640x480, or 320x240 in the 240p mode (gDcVideo240p, dc_video_options.h), which
// the debug menu can switch. Render width = DC_FB_W << the current FSAA mode (FSAA can be switched
// off per course).
static int sFbW = 640, sFbH = 480;
static int sRenderW = 640 << 1;
#define DC_FB_W sFbW
#define DC_FB_H sFbH
#define DC_RENDER_W sRenderW
// These scenes are drawn without FSAA in the curated mode, as FSAA_SCENE(level, area)
// (comma-separated): their fill does not fit 60 fps with FSAA, which doubles it.
#include "pc/dc_video_options.h"
#include "pc/dc_overlay.h"
#include "level_table.h"
#define FSAA_SCENE(level, area) ((level) * 16 + (area))
#ifndef DC_FSAA_OFF_SCENES
// Water and lava, whose TR fill covers much of the screen: WDW 1-2, JRB 1, THI 1, LLL 2, DDD 1-2,
// COTMC 1. And the scenes under 98% of frames on time with FSAA on in a 120-star playthrough:
// VCUTM 97.1%, SL 93.5%, BITDW 97.9%, Bowser 1 94%, and THI 3, the Wiggler's room, 88.6%. TTC holds
// 99.2% with its grid in TR.
#define DC_FSAA_OFF_SCENES                                                                         \
    FSAA_SCENE(LEVEL_WDW, 1), FSAA_SCENE(LEVEL_WDW, 2), FSAA_SCENE(LEVEL_JRB, 1),                  \
    FSAA_SCENE(LEVEL_THI, 1), FSAA_SCENE(LEVEL_THI, 3), FSAA_SCENE(LEVEL_LLL, 2),                  \
    FSAA_SCENE(LEVEL_DDD, 1), FSAA_SCENE(LEVEL_DDD, 2), FSAA_SCENE(LEVEL_COTMC, 1),                \
    FSAA_SCENE(LEVEL_VCUTM, 1), FSAA_SCENE(LEVEL_SL, 1), FSAA_SCENE(LEVEL_BITDW, 1),               \
    FSAA_SCENE(LEVEL_BOWSER_1, 1)
#endif
static void pvr_post_init(void);
static void gfx_pvr_request_fsaa_for_scene(int level, int area);
extern s16 gCurrLevelNum, gCurrAreaIndex;

// ---------------------------------------------------------------------------
// Shader bookkeeping — The front-end treats ShaderProgram opaquely
// (only via shader_get_info), so this layout is
// private to the backend. "Shaders" here will resolve to a small set of
// pvr_poly_cxt configs in P3; for now we only need the CC feature flags.
// ---------------------------------------------------------------------------
enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    uint8_t enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used;   // SM64 is single-texture
    int num_inputs;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size = 0;
static struct ShaderProgram *cur_shader = NULL;

// ---------------------------------------------------------------------------
// PVR state
// ---------------------------------------------------------------------------
static pvr_dr_state_t sDrState;
static int sCurrentList = PVR_LIST_OP_POLY;

// Per-primitive depth state, driven by the front-end (set_depth_test/mask/zmode_decal)
// straight from the RDP other_mode bits — NOT magic shader ids. PVR bakes depth state
// into the compiled poly header, so when any of these change we mark the header dirty
// and lazily recompile+submit a fresh one before the next vertices. This is what makes
// e.g. the skybox (near geometry drawn with Z_UPD off) test-but-not-write, so it no
// longer buries the track. (P3 will cache headers + fold in blend/texture state.)
static uint8_t sDepthTest  = 1;   // PVR_DEPTHCMP_GEQUAL vs ALWAYS
static uint8_t sDepthWrite = 1;   // PVR_DEPTHWRITE_ENABLE vs DISABLE
// (zmode decal is NOT header state — handled as a baked-z nudge in the front-end; see
//  gfx_pvr_set_zmode_decal + PVR_DECAL_ZBIAS in gfx_retro_dc.c)
static uint8_t sTexEnv     = PVR_TXRENV_MODULATE;  // texel<->vtx combine, derived from N64 combiner
static uint8_t sFogEnabled = 0;   // PVR_FOG_VERTEX vs PVR_FOG_DISABLE (G_FOG geometry mode)

// --- Texture table ---------------------------------------------------------
// The front-end hands upload_texture data ALREADY in PVR 16-bit format (ARGB1555
// or ARGB4444 — the "rgba32"/GL_BGRA naming is a leftover lie from the PC port),
// so there is no pixel conversion: we POT-PAD (never resample — see memory), copy
// into VRAM, and the front-end's get_*_scale UV correction addresses the used
// sub-region. Ids are 1-based table indices; 0 == none.
#define PVR_TEX_MAX 1024
struct PvrTex {
    pvr_ptr_t addr;        // VRAM, NULL if unallocated
    uint32_t  alloc_size;  // bytes currently allocated (reused if a re-upload fits)
    uint16_t  w, h;        // padded POT dims
    int       fmt;         // PVR_TXRFMT_ARGB1555/4444 | NONTWIDDLED
    uint8_t   filter;      // 0 = point, 1 = bilinear
    uint8_t   clampu, clampv;   // G_TX_CLAMP -> PVR_UVCLAMP
    uint8_t   flipu, flipv;     // G_TX_MIRROR -> PVR_UVFLIP (mirror-repeat)
    float     u_scale, v_scale; // real/padded, consumed by gfx_pvr_get_*_scale
};
static struct PvrTex sTextures[PVR_TEX_MAX];
// SIX base poly headers — {colour, textured} x {OP, PT, TR} — each compiled ONCE by the official
// pvr_poly_cxt+pvr_poly_compile, so every PER-LIST field (list_type/alpha/txralpha/blend defaults)
// is correct. Tiny + cache-friendly (192 bytes; a per-TEXTURE header array would be ~64KB and
// thrash the SH4 operand cache). pvr_compile_header copies the matching base, then bit-patches the
// texture fields (addr/dims/fmt/filter/clamp/flip/env — OoT-style) + per-draw state (depth/fog/
// TR-blend). No pvr_poly_compile per draw.
static pvr_poly_hdr_t __attribute__((aligned(32))) sBaseHdr[2][3];   // [textured][PVR_KIND_*]
static uint8_t sBaseHdrValid[2][3];
static uint32_t sTexCount = 0;   // high-water id allocator
static uint32_t sBoundTex = 0;   // currently bound texture (SM64 = single texture)
static uint32_t sCurBound = 0;   // last select_texture id == upload target

// --- OP live + PT/TR deferred -----------------------------------------------
// Only ONE list can be open on the DR path at a time, so the OP (opaque) list stays
// open and LIVE the whole frame — geometry the front-end can GUARANTEE is fully opaque
// (pvr_submit_op) streams straight out as it arrives. The other two kinds are buffered
// into per-kind buckets and replayed at end_frame, each into its own list opened once:
//   * PT (sPunch): alpha-TEST cutouts (hard-edged fences/foliage/glyphs). Opaque pixels,
//     depth-tested AND depth-written, alpha-tested by the hardware. Flushed FIRST.
//   * TR (sTR): real alpha-BLEND surfaces. Depth-tested, autosorted per-pixel (pvr_init),
//     composited over OP+PT. Flushed SECOND, so it blends over the cutouts.
// The front-end classifies each primitive (gfx_pvr_set_blend -> sListKind) into OP/PT/TR.
#define TR_MAX_VERTS    8192
// Bowser fights once overflowed this (PVR_DROP batch 2): the front-end re-dirtied the poly header
// per textured triangle, so same-texture translucent tris (coins/flames) fragmented to ~1 tri per
// batch -> ~580 batches. That was fixed at the root in gfx_pvr_set_sampler_parameters (dirty only
// on real sampler-state change); the same scene now measures ~73 batches, so 512 keeps ~7x margin.
#define TR_MAX_BATCH     512

typedef struct {
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    uint32_t start, count;
    uint8_t fade;    // FADE_*: how the frame's fade reaches its vertices (pvr_flush_bucket)
} PvrBatch;
typedef struct {
    pvr_vertex_t *verts; PvrBatch *batch;
    uint32_t nverts, max_verts;
    int nbatch, max_batch, cur;
    uint8_t dirty;   // state changed since this bucket's current batch was compiled
} PvrBucket;

static pvr_vertex_t sTrVerts[TR_MAX_VERTS] __attribute__((aligned(32)));
static PvrBatch     sTrBatch[TR_MAX_BATCH];
static PvrBucket sTR = { sTrVerts, sTrBatch, 0, TR_MAX_VERTS, 0, TR_MAX_BATCH, -1, 1 };

// Deferred PUNCH-THROUGH bucket: alpha-test cutouts (hard-edged, opaque pixels, write
// depth). Flushed at end_frame BEFORE TR.
static pvr_vertex_t sPunchVerts[TR_MAX_VERTS] __attribute__((aligned(32)));
static PvrBatch     sPunchBatch[TR_MAX_BATCH];
static PvrBucket sPunch = { sPunchVerts, sPunchBatch, 0, TR_MAX_VERTS, 0, TR_MAX_BATCH, -1, 1 };

// Routing for the current primitive: 0 = OP (live), 1 = PT (sPunch bucket), 2 = TR (sTR).
#define PVR_KIND_OP 0
#define PVR_KIND_PT 1
#define PVR_KIND_TR 2
static uint8_t sListKind = PVR_KIND_OP;
static uint8_t sOpDirty = 1;   // live OP header needs re-emit (depth/texture changed)

// TR blend factors, derived by the front-end from the N64 blender and pushed via
// gfx_pvr_set_blend_factors. Default = standard alpha-over (SRC_ALPHA / INV_SRC_ALPHA).
static int sBlendSrc = PVR_BLEND_SRCALPHA;
static int sBlendDst = PVR_BLEND_INVSRCALPHA;

// Capped, greppable overflow logging — never drop silently.
static int sDropV = 0, sDropB = 0;

// A depth/texture/shader state change affects the live OP header and the next PT and TR
// batches. (gfx_pvr_set_blend only routes OP/PT/TR, so it does NOT dirty headers.)
static void pvr_mark_dirty(void) { sOpDirty = 1; sTR.dirty = 1; sPunch.dirty = 1; }
// A fade (gfx_dc_tags.h DC_GFX_FADE: the colour C at alpha a over all drawn before its rectangle)
// through the PVR's fog, which mixes a colour into a polygon's pixels after texturing and before
// blending. lerp(pixel, C, a) is the fade for any texture mode, and blending keeps it: a blended
// source over a faded destination is the faded blend (an additive one gains C*a*alpha).
// * A polygon without the game's fog gets TABLE fog: the table is a everywhere, its colour C.
// * One with vertex fog (amount f, colour F) keeps it. Its colours fade in its vertices
//   (rgb*(1-a) + C*a, the C*a on the offset colour when textured) and the vertex fog colour is
//   F*(1-a) + C*a: together the fade of the fogged pixel. REPLACE becomes MODULATE with a grey of
//   1-a; DECAL's texel cannot be scaled, so a fogged DECAL polygon stays a little bright.
// The fog registers are read while the PVR renders, and it renders the frame before while this one
// is sent: they are written at the end of the frame if that render is done, else by its render-done
// interrupt, which comes before this frame's render can begin (fade_registers).
#define FADE_NONE        0
#define FADE_TABLE       1   // the header's fog is the table's now
#define FADE_VTX         2   // vertex fog kept, untextured: the vertex's base colour fades
#define FADE_VTX_TEX     3   // the same, textured: base and offset colour fade, C*a on the offset
#define FADE_VTX_REPLACE 4   // FADE_VTX_TEX, its REPLACE now MODULATE with a white base
static uint8_t sFadeOn, sFadeFrame;   // a fade now / this frame
static uint32_t sFadeRgba;            // 0xRRGGBBAA
static uint8_t sFadeKeep[256];        // c * (255 - a) / 255, rounded down
static uint32_t sFadeAdd;             // C * a / 255 as 0x00RRGGBB, rounded down: no carry with keep
static uint8_t sHdrFade;              // FADE_* of the header pvr_compile_header made last
static uint8_t sOpFade;               // FADE_* of the live OP header
static uint32_t sFogColor;            // the game's vertex fog colour, as the register takes it
static uint32_t sFogRegVertex, sFogRegTable;   // what the registers hold for this frame (sFogRegValid)
static uint8_t sFogRegValid;          // 1 vertex colour, 2 table colour and table
static uint8_t sFogTableUsed;         // a header of this frame has table fog
static uint8_t sFogWant;              // what fade_write writes: 1 vertex colour, 2 the table's
static volatile uint8_t sFogPending;  // fade_write waits for the render running now to end
static volatile uint32_t sRendersDone;   // render-done interrupts (fade_render_done)
static uint32_t sRendersBegun;        // scenes whose render has begun: one each frame start
static uint8_t sSceneSent;            // this frame's scene goes to the PVR
static asic_evt_handler_entry_t sKosRenderDone;   // KOS's handler, called after ours

static inline uint32_t fade_rgb(uint32_t c) {
    return (uint32_t) sFadeKeep[c >> 16 & 0xff] << 16 | (uint32_t) sFadeKeep[c >> 8 & 0xff] << 8 |
           sFadeKeep[c & 0xff];
}

// The fade's change to a header just compiled; FADE_* for its vertices.
static uint8_t fade_header(pvr_poly_hdr_t *h) {
    if (!sFadeOn) return FADE_NONE;
    if (h->m2.fog_type != PVR_FOG_VERTEX) {
        h->m2.fog_type = PVR_FOG_TABLE;
        sFogTableUsed = 1;
        return FADE_TABLE;
    }
    if (!h->m0.txr_en) return FADE_VTX;
    if (h->m2.shading != PVR_TXRENV_REPLACE) return FADE_VTX_TEX;
    h->m2.shading = PVR_TXRENV_MODULATE;
    return FADE_VTX_REPLACE;
}

// A vertex of a FADE_VTX* batch, faded in a copy (the store queues cannot be read back).
static void __attribute__((noinline)) fade_vertex(pvr_vertex_t *v, uint8_t mode) {
    uint32_t base = mode == FADE_VTX_REPLACE ? v->argb | 0x00ffffffu : v->argb;
    base = (base & 0xff000000u) | fade_rgb(base);
    uint32_t off = (v->oargb & 0xff000000u) | fade_rgb(v->oargb);
    if (mode == FADE_VTX) base += sFadeAdd;
    else off += sFadeAdd;
    v->argb = base;
    v->oargb = off;
}

// The frame's fade starts (gfx_retro_dc.c gfx_dc_tag): every header from here on takes it.
void gfx_pvr_fade_begin(uint32_t rgba) {
    uint32_t a = rgba & 0xff;
    sFadeRgba = rgba;
    sFadeOn = sFadeFrame = 1;
    for (uint32_t c = 0; c < 256; c++) sFadeKeep[c] = (uint8_t) (c * (255 - a) / 255);
    sFadeAdd = ((rgba >> 24) * a / 255) << 16 | ((rgba >> 16 & 0xff) * a / 255) << 8 |
               (rgba >> 8 & 0xff) * a / 255;
    // The background plane is no polygon (KOS draws it at the render's start): faded here.
    extern int clear_color;
    float bg[3];
    bg[0] = ((clear_color >> 11) & 0x1F) * (1.0f / 31.0f);
    bg[1] = ((clear_color >> 6) & 0x1F) * (1.0f / 31.0f);
    bg[2] = ((clear_color >> 1) & 0x1F) * (1.0f / 31.0f);
    const float fa = a * (1.0f / 255.0f);
    pvr_set_bg_color(bg[0] + ((rgba >> 24) * (1.0f / 255.0f) - bg[0]) * fa,
                     bg[1] + ((rgba >> 16 & 0xff) * (1.0f / 255.0f) - bg[1]) * fa,
                     bg[2] + ((rgba >> 8 & 0xff) * (1.0f / 255.0f) - bg[2]) * fa);
    pvr_mark_dirty();
}

// Where the fade's rectangle was: what follows is drawn as it is (the pause menu over its shade).
void gfx_pvr_fade_end(void) {
    sFadeOn = 0;
    pvr_mark_dirty();
}

// Writes sFogRegVertex and sFogRegTable as sFogWant says.
static void fade_write(void) {
    if (sFogWant & 1) PVR_SET(PVR_FOG_VERTEX_COLOR, sFogRegVertex);
    if (sFogWant & 2) {
        const uint32_t a = sFogRegTable & 0xff;
        PVR_SET(PVR_FOG_TABLE_COLOR, 0xff000000u | sFogRegTable >> 8);
        for (uint32_t i = 0; i < 128; i++) PVR_SET(PVR_FOG_TABLE_BASE + i * 4, a << 8 | a);
    }
}

// The TSP finished a render (interrupt context): the next frame's fog registers, if they wait.
static void fade_render_done(uint32_t code, UNUSED void *data) {
    if (sFogPending) {
        fade_write();
        sFogPending = 0;
    }
    sRendersDone++;
    if (sKosRenderDone.hdl != NULL) sKosRenderDone.hdl(code, sKosRenderDone.data);
}

// pvr_post_init: after each pvr_init, which hooks KOS's own handler again.
static void fade_hook_render_done(void) {
    asic_evt_handler_entry_t old = asic_evt_set_handler(ASIC_EVT_PVR_RENDERDONE_TSP, fade_render_done, NULL);
    if (old.hdl != fade_render_done) sKosRenderDone = old;
    sFogPending = 0;
    sFogRegValid = 0;   // pvr_init's values: written again at the frame's end
    sRendersBegun = sRendersDone;
    sSceneSent = 0;
}

// gfx_pvr_start_frame, once the PVR takes this frame (pvr_wait_ready: the frame before renders).
static void fade_frame_start(void) {
    if (sSceneSent) sRendersBegun++;
    if (sRendersBegun < sRendersDone) sRendersBegun = sRendersDone;   // a scene not of gfx_pvr
    sSceneSent = 0;
    sFadeOn = sFadeFrame = 0;   // until the display list's DC_GFX_FADE
    sFogTableUsed = 0;
}

// gfx_pvr_end_frame: the fog registers this frame renders with.
static void fade_registers(void) {
    uint32_t vtx = sFogColor;
    if (sFadeFrame) vtx = (vtx & 0xff000000u) | (fade_rgb(vtx) + sFadeAdd);
    const int table = sFadeFrame && sFogTableUsed;
    uint8_t want = 0;
    if (!(sFogRegValid & 1) || vtx != sFogRegVertex) want |= 1;
    if (table && (!(sFogRegValid & 2) || sFadeRgba != sFogRegTable)) want |= 2;
    sSceneSent = 1;
    if (!want) return;
    sFogRegVertex = vtx;
    if (want & 2) sFogRegTable = sFadeRgba;
    sFogRegValid |= want;
    const irq_mask_t irq = irq_disable();
    sFogWant = want;
    if (sRendersBegun > sRendersDone) sFogPending = 1;   // the frame before still renders
    else fade_write();
    irq_restore(irq);
}

// Compile one of the SIX base headers ONCE via the official pvr_poly_cxt+pvr_poly_compile, so
// every PER-LIST field (list_type/alpha/txralpha/blend defaults) is correct for this list. The
// textured bases seed from whatever texture is bound on first use; their texture-specific fields
// get overwritten per draw, so the seed texture is irrelevant.
static void pvr_ensure_base(int textured, int kind, int list) {
    if (sBaseHdrValid[textured][kind]) return;
    pvr_poly_cxt_t cxt;
    if (textured) {
        struct PvrTex *t = &sTextures[sBoundTex];
        pvr_poly_cxt_txr(&cxt, list, t->fmt, t->w, t->h, t->addr,
                         t->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }
    // SMALL (not NONE): the front-end already does N64 backface culling (G_CULL_* in
    // gfx_sp_tri1), so HW culling isn't needed for winding — but SMALL also discards
    // degenerate / near-zero-area triangles the front-end lets through, which were
    // producing visible degen artifacts. Winding-direction culling stays off via this.
    cxt.gen.culling  = PVR_CULLING_SMALL;
    cxt.gen.specular = PVR_SPECULAR_ENABLE;  // combiner routes additive offsets into oargb
    pvr_poly_compile(&sBaseHdr[textured][kind], &cxt);
    sBaseHdrValid[textured][kind] = 1;
}

// Build a poly header for the current state. pvr_poly_compile runs only ONCE per base (6 total);
// per draw this copies the matching base and bit-patches the texture fields + per-draw state
// in place (OoT technique) — no per-draw recompile. Per-list state stays from the base.
// A native segment's precompiled header (gfx_native.inc.c), used for its kind instead of the
// current state while the segment draws.
static const pvr_poly_hdr_t *sForcedHdr[3];

static void pvr_compile_header_raw(pvr_poly_hdr_t *out, int kind) {
    if (sForcedHdr[kind] != NULL) {
        *out = *sForcedHdr[kind];
        return;
    }
    int list = (kind == PVR_KIND_TR) ? PVR_LIST_TR_POLY
             : (kind == PVR_KIND_PT) ? PVR_LIST_PT_POLY
             :                         PVR_LIST_OP_POLY;
    int textured = cur_shader && cur_shader->texture_used &&
                   sBoundTex && sTextures[sBoundTex].addr;
    pvr_ensure_base(textured, kind, list);
    *out = sBaseHdr[textured][kind];

    if (textured) {
        struct PvrTex *t = &sTextures[sBoundTex];
        // Texture fields, encoded exactly as KOS pvr_poly_compile (pvr_prim.c): mode3 =
        // format | ((addr & 0x00fffff8) >> 3); mode2 USIZE/VSIZE = ctz(dim)-3; filter/clamp/flip.
        out->mode3 = (uint32_t) t->fmt |
                     ((((uint32_t)(uintptr_t) t->addr) & 0x00fffff8u) >> 3);
        out->m2.u_size      = (pvr_uv_size_t)(__builtin_ctz(t->w) - 3);
        out->m2.v_size      = (pvr_uv_size_t)(__builtin_ctz(t->h) - 3);
        out->m2.filter_mode = t->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE;
        out->m2.u_clamp     = t->clampu;
        out->m2.v_clamp     = t->clampv;
        out->m2.u_flip      = t->flipu;
        out->m2.v_flip      = t->flipv;
        // texenv DERIVED from the N64 combiner (REPLACE/MODULATE/DECAL/MODULATEALPHA).
        out->m2.shading     = (pvr_txr_shading_mode_t) sTexEnv;
    }
    // Per-draw state (per-list alpha/txralpha/blend defaults stay from the base):
    // z = 1/w (larger == nearer): GEQUAL keeps the nearer fragment; ALWAYS == test off.
    out->m1.depth_cmp       = sDepthTest  ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    out->m1.depth_write_dis = sDepthWrite ? 0 : 1;
    // PVR vertex fog reads density from per-vertex oargb ALPHA (front-end packs it); off clears.
    out->m2.fog_type        = sFogEnabled ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    if (kind == PVR_KIND_TR) {
        // Only TR overrides blend (factors DERIVED from the N64 blender). OP/PT keep the base's
        // per-list defaults (ONE/ZERO and SRCALPHA/INVSRCALPHA).
        out->m2.blend_src = (pvr_blend_mode_t) sBlendSrc;
        out->m2.blend_dst = (pvr_blend_mode_t) sBlendDst;
    }
}

// The header the current state (or the native renderer's forced header) draws with, with the fade.
static void pvr_compile_header(pvr_poly_hdr_t *out, int kind) {
    pvr_compile_header_raw(out, kind);
    sHdrFade = fade_header(out);
}

#define PROF_BATCH_ALPHA(bt, kind) ((void) 0)

// Append n vertices (n/3 tris) of already-screen-baked dc_fast_t to a bucket,
// starting a new batch when the bucket's header state has changed.
static void pvr_append(PvrBucket *b, int kind, const dc_fast_t *tris, size_t n) {
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
        b->batch[b->cur].fade = sHdrFade;
        PROF_BATCH_ALPHA(&b->batch[b->cur], kind);
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + n > b->max_verts) { if (sDropV++ < 8) printf("PVR_DROP verts %d\n", kind); return; }
    for (size_t i = 0; i < n; i++) {
        pvr_vertex_t *v = &b->verts[b->nverts++];
        *v = *(const pvr_vertex_t *) &tris[i];
        v->flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        // oargb (additive offset) carried through dc_fast_t.pad0 — do not zero.
    }
    b->batch[b->cur].count += n;
}

// Reserve n verts in the deferred bucket for `kind` (1=PT/sPunch, 2=TR/sTR) and return a write
// pointer into b->verts so the FRONT-END bakes vertices straight into the bucket — no buf_vbo, no
// copy. Starts a new batch (header compiled from current state) when the bucket is dirty, exactly
// like pvr_append. Vertex flags are stamped at flush time (pvr_flush_bucket). NULL on overflow.
dc_fast_t *pvr_reserve(int kind, size_t n) {
    PvrBucket *b = (kind == PVR_KIND_TR) ? &sTR : &sPunch;
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return NULL; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
        b->batch[b->cur].fade = sHdrFade;
        PROF_BATCH_ALPHA(&b->batch[b->cur], kind);
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + n > b->max_verts) { if (sDropV++ < 8) printf("PVR_DROP verts %d\n", kind); return NULL; }
    dc_fast_t *out = (dc_fast_t *) &b->verts[b->nverts];
    b->nverts += n;
    b->batch[b->cur].count += n;
    // Stamp strip flags now: the front-end bakes vert/uv/colour into this slice but never flags. These
    // are TRIANGLES (n == n_tris*3), so EOL on every 3rd vertex. (2D quads set their own 4-vert flags.)
    for (size_t i = 0; i < n; i++)
        out[i].flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    return out;
}

#define HASH_OP_HEADER(h) ((void) 0)
#define HASH_VERTEX(kind, hdr, v, flags) ((void) 0)

// Send one store queue's 32 bytes to the TA. Unlike pvr_dr_commit (sq_flush) this has no "memory"
// clobber: the asm only reads this vertex, so GCC keeps everything else in registers around it.
static inline void sq_send(pvr_vertex_t *v) {
    __asm__ __volatile__("pref @%0" : : "r"(v), "m"(*v));
}

// One store-queue vertex: pvr_dr_target's double-buffered address, kept in a register by the
// caller instead of the global pvr_dr_addr being loaded and stored per vertex.
static inline pvr_vertex_t *sq_next(uint32_t *addr) {
    *addr ^= 32;
    return (pvr_vertex_t *) __builtin_assume_aligned((void *) *addr, 32);
}

// Replay a bucket's batches (header + its verts) to the open list via the DR path.
static void pvr_flush_bucket(PvrBucket *b) {
    uint32_t addr = pvr_dr_addr;
    for (int i = 0; i < b->nbatch; i++) {
        PvrBatch *bt = &b->batch[i];
        if (!bt->count) continue;
        pvr_poly_hdr_t *hp = (pvr_poly_hdr_t *) sq_next(&addr);
        *hp = bt->hdr;
        sq_send((pvr_vertex_t *) hp);
        const pvr_vertex_t *src = &b->verts[bt->start];
        if (bt->fade >= FADE_VTX) {
            for (uint32_t v = 0; v < bt->count; v++) {
                pvr_vertex_t t = src[v];
                fade_vertex(&t, bt->fade);
                pvr_vertex_t *vp = sq_next(&addr);
                *vp = t;
                sq_send(vp);
            }
            continue;
        }
        for (uint32_t v = 0; v < bt->count; v++) {
            pvr_vertex_t *vp = sq_next(&addr);
            *vp = src[v];
            // Flags are already set in the bucket: 3-vert tris by pvr_reserve, 4-vert quads by
            // gfx_pvr_draw_quad_2d. Do NOT re-stamp here — a blanket (v%3) would corrupt the 2D quads.
            sq_send(vp);
            HASH_VERTEX(b == &sTR ? PVR_KIND_TR : PVR_KIND_PT, hh, &src[v], src[v].flags);
        }
    }
    pvr_dr_addr = addr;
}

// Scratch for POT padding (max 256x256 @ 16bpp = 128KB). MK64 textures are small.
static uint16_t sPadScratch[256 * 256] __attribute__((aligned(32)));

static inline uint32_t pvr_next_pot(uint32_t v) {
    v--; v |= v>>1; v |= v>>2; v |= v>>4; v |= v>>8; v |= v>>16; return v+1;
}
static inline int pvr_is_pot(uint32_t v) { return (v & (v - 1)) == 0; }

// Pad-copy with clamp-to-edge): real image at top-left,
// pad columns/rows replicate the edge so bilinear at the border samples real texels.
static void pvr_pad16(const uint16_t *in, int iw, int ih, uint16_t *out, int ow, int oh) {
    int y;

    for (y = 0; y < ih; y++) {
        uint16_t *o = out + (y * ow);
        shz_memcpy(o, in + (y * iw), iw * 2);
        uint16_t edge = (iw > 0) ? in[(y * iw) + iw - 1] : 0;
        for (int x = iw; x < ow; x++) o[x] = edge;
    }
    if (ih > 0) {
        const uint16_t *last = out + ((ih - 1) * ow);
        for (; y < oh; y++) shz_memcpy(out + (y * ow), last, ow * 2);
    }
}


// Mirror of OoT's known-good params for this toolchain:
//   {OP, OP_MOD, TR, TR_MOD, PT} bin sizes, vtxbuf, dma, fsaa, autosort_disabled, overflow
// Live list is OP (guaranteed-opaque geometry). Alpha-TEST CUTOUTS (hard-edged fences/
// foliage/glyphs — texture_edge or texture-driven alpha) go to the deferred PT bucket;
// real alpha-BLEND surfaces go to the deferred TR bucket. At end_frame PT is flushed first
// (writes depth, alpha-tested opaque), then TR composites over it. All three lists' bins
// are live -> {OP, OP_MOD, TR, TR_MOD, PT}.
#ifndef DC_PT_CHUNK
#define DC_PT_CHUNK 64
#endif
_Static_assert(DC_PT_CHUNK >= 32 && DC_PT_CHUNK <= 512, "ISP_FEED_CFG punch-through chunk, vertices");
static pvr_init_params_t sPvrParams = {
    { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
    512 * 1024,
    1,  // dma
    1,  // fsaa: horizontal supersampling (DC_RENDER_W wide)
    0,  // autosort_disabled (0 = autosort; presort measured no faster with FSAA, 2026-10-24)
    2,  // overflow buffers
    0,  // vbuf_doublebuf_disabled (0 = double-buffering on)
};

// ===========================================================================
// GfxRenderingAPI implementation
// ===========================================================================

static void gfx_pvr_unload_shader(UNUSED struct ShaderProgram *old_prg) {
    cur_shader = NULL;
}

static void gfx_pvr_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    pvr_mark_dirty();   // textured-vs-colour header depends on the combiner
}

static struct ShaderProgram *gfx_pvr_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used = ccf.used_texture;   // SM64 single-texture CCFeatures

    if (ccf.used_texture && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_texture) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    } else {
        prg->mix = SH_MT_NONE;
    }

    prg->enabled = 0;
    gfx_pvr_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_pvr_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static bool gfx_pvr_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs) {
    *num_inputs = prg->num_inputs;
    return prg->texture_used;   // SM64 rapi: returns the single used-texture flag
}

static uint32_t gfx_pvr_new_texture(void) {
    uint32_t id = ++sTexCount;
    // Clamp guards the array, but a clamp means the id space is exhausted THIS reset cycle:
    // every further texture aliases slot 1023 -> "same texture everywhere". sTexCount resets in
    // gfx_pvr_clear_all_textures, so this should not fire; if it does, a single course needs
    // >PVR_TEX_MAX live textures. Don't fail silently.
    if (id >= PVR_TEX_MAX) {
        static int sTexIdClampLog = 0;
        if (sTexIdClampLog++ < 8) printf("PVR_TEXID_CLAMP sTexCount=%u >= PVR_TEX_MAX=%u\n", (unsigned)id, (unsigned)PVR_TEX_MAX);
        id = PVR_TEX_MAX - 1;
    }
    memset(&sTextures[id], 0, sizeof(sTextures[id]));
    return id;
}

static void gfx_pvr_select_texture(uint32_t texture_id) {
    // Dirty the poly header only when the binding actually CHANGES. The front-end's texture cache
    // calls this on every lookup (~195/frame), overwhelmingly re-binding the already-bound texture;
    // an unconditional dirty forced a header re-emit + PT/TR batch split per lookup. Re-uploads
    // dirty for themselves in gfx_pvr_upload_texture, so a same-id rebind is header-neutral.
    // (Matches mk64-dc/sf64-dc, which already had this guard; SM64 was missing it.)
    uint8_t changed = (sBoundTex != texture_id);
    sCurBound = texture_id;   // upload target follows the most recent bind
    sBoundTex = texture_id;
    if (changed)
        pvr_mark_dirty();
}

static void gfx_pvr_upload_texture(const uint8_t *buf16, int width, int height, unsigned int type) {
    struct PvrTex *t = &sTextures[sCurBound];
    // Twiddle every texture. SM64's cache uploads each texture ONCE per area (miss -> upload; hits
    // reuse; whole set nuked at area/course reset), so the Morton reorder is a one-time cost, not
    // per-frame like MK64. N64 TMEM (4KB) caps every single load far under the pad scratch, so all
    // uploads end up POT after padding -- a non-twiddled/NPOT path is unreachable here.
    int fmt = type;   // twiddled == no PVR_TXRFMT_NONTWIDDLED bit

    const uint16_t *src = (const uint16_t *) buf16;
    uint32_t fw = width, fh = height;
    float us = 1.0f, vs = 1.0f;

    if (!pvr_is_pot(width) || !pvr_is_pot(height) || width < 8 || height < 8) {
        fw = (width  < 8) ? 8 : (pvr_is_pot(width)  ? (uint32_t) width  : pvr_next_pot(width));
        fh = (height < 8) ? 8 : (pvr_is_pot(height) ? (uint32_t) height : pvr_next_pot(height));
        // Gate on TOTAL texels vs the scratch capacity, not per-dimension: the title
        // background's 512x8 strips (512-wide, short) fit fine by area but a width cap
        // would wrongly reject them -> NPOT upload -> static noise over the screen.
        if ((uint32_t) fw * (uint32_t) fh <= 256u * 256u) {
            pvr_pad16(src, width, height, sPadScratch, fw, fh);
            src = sPadScratch;
            us = (float) width  / (float) fw;
            vs = (float) height / (float) fh;
        } else {
            fw = width; fh = height;   // genuinely too big for scratch: upload as-is (rare)
        }
    }

    uint32_t size = fw * fh * 2;
    // Mirror glTexImage2D: reuse the existing VRAM allocation when the new upload fits,
    // only (re)allocate when it's bigger. The whole texture VRAM set is freed at the
    // glDeleteTextures point (gfx_pvr_clear_all_textures, via nuke_everything).
    if (t->addr && size > t->alloc_size) { pvr_mem_free(t->addr); t->addr = NULL; }
    if (!t->addr) { t->addr = pvr_mem_malloc(size); t->alloc_size = t->addr ? size : 0; }
    if (t->addr) pvr_txr_load_ex((void *) src, t->addr, fw, fh, PVR_TXRLOAD_16BPP);

    t->w = fw; t->h = fh; t->fmt = fmt;
    t->u_scale = us; t->v_scale = vs;
    pvr_mark_dirty();
}

static void gfx_pvr_set_sampler_parameters(bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct PvrTex *t = &sTextures[sBoundTex];
    // Match GLdc gfx_cm_to_opengl precedence: CLAMP wins, else MIRROR (mirror-repeat),
    // else plain repeat. PVR clamp == GL_CLAMP, PVR flip == GL_MIRRORED_REPEAT.
    uint8_t filter = linear_filter ? 1 : 0;
    uint8_t clampu = (cms & G_TX_CLAMP) ? 1 : 0;
    uint8_t clampv = (cmt & G_TX_CLAMP) ? 1 : 0;
    uint8_t flipu  = (!(cms & G_TX_CLAMP) && (cms & G_TX_MIRROR)) ? 1 : 0;
    uint8_t flipv  = (!(cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR)) ? 1 : 0;
    // The front-end calls this for EVERY textured primitive (gfx_sp_tri1/quad/texrect), not just on
    // change. Dirtying unconditionally re-emitted the poly header per triangle: a run of same-texture
    // translucent tris (coins/flames in Bowser fights) fragmented into ONE TR batch PER TRIANGLE
    // (~580 measured), and every opaque textured tri needlessly re-emitted its OP header. Only dirty
    // when THIS texture's sampler state actually changes; texture-IDENTITY changes are already
    // dirtied by gfx_pvr_select_texture, so same-state runs now coalesce into one batch/header.
    if (t->filter == filter && t->clampu == clampu && t->clampv == clampv
            && t->flipu == flipu && t->flipv == flipv) {
        return;
    }
    t->filter = filter; t->clampu = clampu; t->clampv = clampv; t->flipu = flipu; t->flipv = flipv;
    pvr_mark_dirty();
}

// Free ALL cached texture VRAM. Called from nuke_everything at course/memory resets) — the
// point where the texture cache's VRAM (including reuse bloat) is reclaimed.
//
// CRITICAL: also RESET the id allocator. nuke_everything calls this and then
// reset_texcache() (front-end cache wipe), after which the front-end re-requests a fresh
// id (new_texture -> ++sTexCount) for every texture it re-encounters. If sTexCount is NOT
// reset here, it climbs monotonically across course/memory resets; once cumulative unique
// textures exceed PVR_TEX_MAX, new_texture clamps EVERY further id to slot 1023 and the
// whole scene draws with the last-uploaded texture (the "same texture everywhere" wedge,
// never recovers without restart). GLdc never hit this — its id space isn't capped.
void gfx_pvr_clear_all_textures(void) {
    for (uint32_t i = 0; i <= sTexCount && i < PVR_TEX_MAX; i++) {
        if (sTextures[i].addr) {
            pvr_mem_free(sTextures[i].addr);
            sTextures[i].addr = NULL;
            sTextures[i].alloc_size = 0;
        }
    }
    sTexCount = 0;   // realign with reset_texcache() (front-end), which runs right after
    sBoundTex = 0;
    sCurBound = 0;
}

extern void reset_texcache(void);

void nuke_everything(void) {
    gfx_pvr_clear_all_textures();
    reset_texcache();
}


// PoT-padding UV correction (real/padded), consumed by the front-end's recip_tex_*
// in place of GLdc's get_current_*_scale (PVR build routes to these).
float gfx_pvr_get_u_scale(void) { return sTextures[sBoundTex].u_scale; }
float gfx_pvr_get_v_scale(void) { return sTextures[sBoundTex].v_scale; }

static void gfx_pvr_set_depth_test(bool depth_test) {
    if (sDepthTest != depth_test) { sDepthTest = depth_test; pvr_mark_dirty(); }
}
static void gfx_pvr_set_depth_mask(bool z_upd) {
    if (sDepthWrite != z_upd) { sDepthWrite = z_upd; pvr_mark_dirty(); }
}
static void gfx_pvr_set_tex_env(uint32_t mode) {
    // enum gfx_tex_env (front-end) == pvr_txr_shading_mode order, so store straight through.
    if (sTexEnv != (uint8_t) mode) { sTexEnv = (uint8_t) mode; pvr_mark_dirty(); }
}

static void gfx_pvr_set_zmode_decal(UNUSED bool zmode_decal) {
    // N64 ZMODE_DEC decal z-fighting is handled entirely in the front-end bake (gfx_sp_tri1):
    // a decal vert's z (1/w) is nudged nearer by PVR_DECAL_ZBIAS so it wins the GEQUAL depth
    // test against its coplanar base. No PVR poly-header change is needed, so this is a no-op —
    // which also avoids recompiling the header every time decal mode toggles (frequent).
}

// PVR vertex fog enable/disable — baked into the poly header, so a change dirties all lists.
void gfx_pvr_set_fog(uint8_t enabled) {
    enabled = enabled ? 1 : 0;
    if (sFogEnabled != enabled) { sFogEnabled = enabled; pvr_mark_dirty(); }
}

// Vertex fog colour register (0x00b4). Global, read at render time — write it directly
// (KOS's pvr_fog_vertex_color is a stub). Not part of the poly header, so no dirty needed.
void gfx_pvr_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    sFogColor = (uint32_t) a << 24 | (uint32_t) r << 16 | (uint32_t) g << 8 | b;   // fade_registers writes it
}
static void gfx_pvr_set_viewport(UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h) { /* front-end SW viewport */ }
static void gfx_pvr_set_scissor(UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h)  { /* front-end SW scissor */ }
static void gfx_pvr_set_use_alpha(UNUSED bool use_alpha)        { /* OP/PT/TR routing comes via gfx_pvr_set_blend */ }

// Front-end list classifier, called directly (not via rapi) from gfx_sp_tri1/gfx_sp_quad_2d:
// kind 0 = OP (fully opaque -> live list), 1 = PT (alpha-test cutout -> sPunch bucket),
// 2 = TR (real alpha-blend -> sTR bucket). Routing only — does NOT dirty headers (each
// list's header is its own identity + depth/texture state). Persists across frames in
// lockstep with the front-end's tracker, so start_frame does not reset it.
void gfx_pvr_set_blend(uint8_t kind) {
    sListKind = kind;   // 0=OP (live), 1=PT (sPunch bucket), 2=TR (sTR bucket)
}

// Map the front-end's abstract gfx_blend_factor codes to PVR blend modes for the TR list.
// Only TR blends, so a change dirties only the TR header (not OP/PT). No shader-id keying.
void gfx_pvr_set_blend_factors(uint8_t src_code, uint8_t dst_code) {
    static const int kMap[] = {
        [GFX_BLENDF_ZERO]        = PVR_BLEND_ZERO,
        [GFX_BLENDF_ONE]         = PVR_BLEND_ONE,
        [GFX_BLENDF_SRCALPHA]    = PVR_BLEND_SRCALPHA,
        [GFX_BLENDF_INVSRCALPHA] = PVR_BLEND_INVSRCALPHA,
        [GFX_BLENDF_DSTALPHA]    = PVR_BLEND_DESTALPHA,
        [GFX_BLENDF_INVDSTALPHA] = PVR_BLEND_INVDESTALPHA,
    };
    int s = kMap[src_code], d = kMap[dst_code];
    if (s != sBlendSrc || d != sBlendDst) {
        sBlendSrc = s; sBlendDst = d;
        sTR.dirty = 1;   // blend state lives only in the TR header
#ifdef PVR_BLEND_DEBUG
        static int n = 0;
        if (n++ < 48)
            printf("BLENDDBG sid=%08lx src=%d dst=%d\n",
                   (unsigned long)(cur_shader ? cur_shader->shader_id : 0), s, d);
#endif
    }
}

#define PROF_COUNT_OP(a, b, c) ((void) 0)

// The native renderer (gfx_native.inc.c). A segment is compiled with the header the current state
// gives (gfx_pvr_header_for) and later drawn with that header (gfx_pvr_native_header) without the
// state; gfx_pvr_native_end hands the lists back to the current state.
void gfx_pvr_header_for(int kind, pvr_poly_hdr_t *out) {
    pvr_compile_header_raw(out, kind);   // kept: a fade reaches it when it is drawn
}

// (depth_test: the header has it as its depth compare here; the host's needs it apart.)
void gfx_pvr_native_header(int kind, const pvr_poly_hdr_t *hdr, UNUSED int depth_test) {
    sForcedHdr[kind] = hdr;
    if (kind == PVR_KIND_OP) sOpDirty = 1;
    else if (kind == PVR_KIND_PT) sPunch.dirty = 1;
    else sTR.dirty = 1;
}

void gfx_pvr_native_end(void) {
    sForcedHdr[0] = sForcedHdr[1] = sForcedHdr[2] = NULL;
    pvr_mark_dirty();
}

// (Re)emit the live OP header to the open OP list when depth/texture state changed.
static inline void pvr_emit_op_header(void) {
    if (!sOpDirty) return;
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    pvr_compile_header(&hdr, PVR_KIND_OP);
    sOpFade = sHdrFade;
    HASH_OP_HEADER(&hdr);
    pvr_poly_hdr_t *hp = (pvr_poly_hdr_t *) pvr_dr_target(sDrState);
    *hp = hdr;
    pvr_dr_commit(hp);
    sOpDirty = 0;
}

// Stream OP geometry straight to the live OP list via DR. Called per source-triangle from the
// front-end (no buf_vbo) AND internally. Header re-emitted only on state change (sOpDirty), so a
// same-state run streams headerless after the first. External — no wrapper needed. (oargb carried
// through dc_fast_t.pad0 — do NOT zero it here.)
void pvr_submit_op(const dc_fast_t *tris, size_t n) {
    pvr_emit_op_header();
    for (size_t i = 0; i < n; i++) {
        pvr_vertex_t *v = (pvr_vertex_t *) pvr_dr_target(sDrState);
        if (sOpFade >= FADE_VTX) {
            pvr_vertex_t t = *(const pvr_vertex_t *) &tris[i];
            fade_vertex(&t, sOpFade);
            *v = t;
        } else
        *v = *(const pvr_vertex_t *) &tris[i];
        v->flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        HASH_VERTEX(PVR_KIND_OP, sHashOpHdr, &tris[i], ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
        pvr_dr_commit(v);
    }
}

// pvr_submit_op_tri under a fade of vertex-fogged polygons, out of .sub0.
static void __attribute__((noinline, cold)) op_tri_faded(const dc_fast_t *a, const dc_fast_t *b,
                                                         const dc_fast_t *c) {
    const dc_fast_t t[3] = { *a, *b, *c };
    pvr_submit_op(t, 3);   // fades its vertices
}

// A baked vertex into the store queue at v with its command word, and sent: as sh4zam's
// shz_sq_memcpy32_1 (four 64-bit moves in and out instead of eight words each), the command word
// written over the first before the queue fires. The size switch (fschg) is inside the one asm, so
// no FPU instruction of the compiler's can land between. src is 32-byte aligned (the baked arrays).
static inline __attribute__((always_inline)) void sq_put_vertex(pvr_vertex_t *v, const dc_fast_t *src, uint32_t flags) {
#ifdef __sh__
    const dc_fast_t *s = src;
    pvr_vertex_t *d = v;
    __asm__ __volatile__(
        "fschg\n\t"
        "fmov.d @%[s]+, dr0\n\t"
        "fmov.d @%[s]+, dr2\n\t"
        "fmov.d @%[s]+, dr4\n\t"
        "fmov.d @%[s]+, dr6\n\t"
        "add    #32, %[d]\n\t"
        "fmov.d dr6, @-%[d]\n\t"
        "fmov.d dr4, @-%[d]\n\t"
        "fmov.d dr2, @-%[d]\n\t"
        "fmov.d dr0, @-%[d]\n\t"
        "fschg\n\t"
        "mov.l  %[f], @%[d]\n\t"
        "pref   @%[d]"
        : [s] "+r" (s), [d] "+r" (d), "=m" (*(uint8_t (*)[32]) v)
        : [f] "r" (flags), "m" (*(const uint8_t (*)[32]) src)
        : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7");
#else
    *v = *(const pvr_vertex_t *) src;
    v->flags = flags;
    sq_send(v);
#endif
}

// One triangle from three separately stored vertices (the front-end's baked-vertex cache).
__attribute__((section(".sub0.sm64_hot"))) void pvr_submit_op_tri(const dc_fast_t *a, const dc_fast_t *b,
                                                                   const dc_fast_t *c) {
    pvr_emit_op_header();
    if (__builtin_expect(sOpFade >= FADE_VTX, 0)) {
        op_tri_faded(a, b, c);
        return;
    }
    uint32_t addr = pvr_dr_addr;
    HASH_VERTEX(PVR_KIND_OP, sHashOpHdr, a, PVR_CMD_VERTEX);
    sq_put_vertex(sq_next(&addr), a, PVR_CMD_VERTEX);
    HASH_VERTEX(PVR_KIND_OP, sHashOpHdr, b, PVR_CMD_VERTEX);
    sq_put_vertex(sq_next(&addr), b, PVR_CMD_VERTEX);
    HASH_VERTEX(PVR_KIND_OP, sHashOpHdr, c, PVR_CMD_VERTEX_EOL);
    sq_put_vertex(sq_next(&addr), c, PVR_CMD_VERTEX_EOL);
    pvr_dr_addr = addr;
    PROF_COUNT_OP((const pvr_vertex_t *) a, (const pvr_vertex_t *) b, (const pvr_vertex_t *) c);
}

// 2D quad depth counter owned by the front-end (gfx_draw_rectangle increments it).
// GLdc's start_frame reset it each frame; the PVR backend must too, or it grows
// unbounded. Positive base because PVR z is inverse-depth (negative would clip).
extern float screen_2d_z;
extern float gfx_frame_invw_max;

// A transition cover starts (gfx_dc_tags.h, gfx_retro_dc.c gfx_dc_tag). Its OP polys write depth
// above world_z_max, the frame's largest world 1/w, so the scene's PT and TR fail the depth test
// under it and cost no fill. 2D drawn before the cover in PT/TR (the HUD) would land on top of it:
// those lists composite after OP, with the depth test off. As on the N64 the cover must hide it
// and the transition's hole must show it, so lift that 2D (the z >= 1.0 overlay band; far-pinned
// backdrops stay) above the world and below the cover, and depth-test it. 2D drawn after the
// cover continues above it through screen_2d_z.
void gfx_pvr_cover_begin(float world_z_max) {
    PvrBucket *buckets[2] = { &sPunch, &sTR };
    for (int k = 0; k < 2; k++) {
        PvrBucket *b = buckets[k];
        for (int i = 0; i < b->nbatch; i++) {
            PvrBatch *bt = &b->batch[i];
            pvr_vertex_t *v = &b->verts[bt->start];
            uint32_t n = bt->count, j;
            if (bt->hdr.m1.depth_cmp != PVR_DEPTHCMP_ALWAYS || !n) continue;
            for (j = 0; j < n && v[j].z >= 1.0f; j++) {}
            if (j < n) continue;   // not all in the overlay band: leave the batch as it is
            for (j = 0; j < n; j++) v[j].z += world_z_max;
            bt->hdr.m1.depth_cmp = PVR_DEPTHCMP_GEQUAL;
        }
        b->dirty = 1;   // later 2D starts a batch with its own (test-off) header
    }
    screen_2d_z += world_z_max + 1.0f;
}

static void gfx_pvr_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len,
                                   size_t buf_vbo_num_tris) {
    // STAGE 3a: front-end now bakes screen-space dc_fast_t (gfx_sp_tri1 PVR path). dc_fast_t
    // shares pvr_vertex_t's 32-byte layout, so each vert is a direct SQ submit. The front-end
    // doesn't classify yet (stage 3b), so sListKind stays OP -> all geometry is opaque for now.
    const dc_fast_t *tris = (const dc_fast_t *) buf_vbo;
    const size_t n = buf_vbo_num_tris * 3;
    if (sListKind == PVR_KIND_TR)
        pvr_append(&sTR, PVR_KIND_TR, tris, n);
    else if (sListKind == PVR_KIND_PT)
        pvr_append(&sPunch, PVR_KIND_PT, tris, n);
    else
        pvr_submit_op(tris, n);   // OP, live
}

// 2D path (called directly by gfx_sp_quad_2d, not via the rapi table). The front-end builds
// the 4 screen-space quad verts ALREADY in PVR triangle-STRIP order (ul, ll, ur, lr), so this
// just emits them as one native 4-vertex strip (v3 = EOL -> 2 tris). Routing via
// gfx_pvr_set_blend: opaque HUD -> live OP; alpha-test glyphs -> PT; real alpha-blend -> TR.
static void gfx_pvr_draw_quad_2d(void *quad, UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    const dc_fast_t *q = (const dc_fast_t *) quad;

    if (sListKind == PVR_KIND_OP) {
        pvr_emit_op_header();
        for (int i = 0; i < 4; i++) {
            pvr_vertex_t *o = (pvr_vertex_t *) pvr_dr_target(sDrState);
            if (sOpFade >= FADE_VTX) {
                pvr_vertex_t t = *(const pvr_vertex_t *) &q[i];
                fade_vertex(&t, sOpFade);
                *o = t;
            } else
            *o = *(const pvr_vertex_t *) &q[i];
            o->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            HASH_VERTEX(PVR_KIND_OP, sHashOpHdr, &q[i], (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
            pvr_dr_commit(o);
        }
        PROF_COUNT_OP((const pvr_vertex_t *) &q[0], (const pvr_vertex_t *) &q[1], (const pvr_vertex_t *) &q[2]);
        PROF_COUNT_OP((const pvr_vertex_t *) &q[1], (const pvr_vertex_t *) &q[2], (const pvr_vertex_t *) &q[3]);
        return;
    }

    PvrBucket *b = (sListKind == PVR_KIND_TR) ? &sTR : &sPunch;
    int kind     = (sListKind == PVR_KIND_TR) ? PVR_KIND_TR : PVR_KIND_PT;
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
        b->batch[b->cur].fade = sHdrFade;
        PROF_BATCH_ALPHA(&b->batch[b->cur], kind);
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + 4 > b->max_verts) { if (sDropV++ < 8) printf("PVR_DROP verts %d\n", kind); return; }
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t *o = &b->verts[b->nverts++];
        *o = *(const pvr_vertex_t *) &q[i];
        o->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    }
    b->batch[b->cur].count += 4;
}

// The 240p mode (gDcVideo240p): the video mode it is shown in. NTSC 320x240 is progressive (240p);
// on VGA it is line-doubled. 640x480 is interlaced on a TV. KOS's pvr_init sets the vertical
// scaler by the mode: the flicker filter only for interlaced TV output.
static int sVideo240p;   // the mode the PVR runs in
int gDcVideo240p = DC_VIDEO_240P;

static int pvr_video_mode(int lo) {
    const int vga = vid_check_cable() == CT_VGA;
    return lo ? (vga ? DM_320x240_VGA : DM_320x240_NTSC) : (vga ? DM_640x480_VGA : DM_640x480_NTSC_IL);
}

static void pvr_fb_size(int lo) {
    sVideo240p = lo;
    sFbW = lo ? 320 : 640;
    sFbH = lo ? 240 : 480;
    sRenderW = sFbW << sPvrParams.fsaa_enabled;
}

static void gfx_pvr_init(void) {
    // enDjinn sets the video mode and starts the PVR, with this port's mode and parameters. AICAflow
    // or audio_dc keeps the AICA, the port reads the controllers itself, and there is no soft reset.
    enj_state_init_defaults();
    enj_state_t *state = enj_state_get();
    // The same specific modes as below, not a generic one (which takes PAL's 50 Hz on a PAL
    // console): enDjinn's field has the generic type, but vid_set_mode takes either.
    pvr_fb_size(gDcVideo240p);
    state->video.display_mode = (vid_display_mode_generic_t) pvr_video_mode(gDcVideo240p);
    state->video.pixel_mode = PM_RGB888;   // XRGB8888, 32-bit (KOS PM_RGB0888): measured 1% faster to render than packed 24-bit
    state->video.pvr_params = sPvrParams;
    state->flags.soft_reset_enabled = 0;
    state->flags.no_snd_init = 1;
    state->flags.no_input_init = 1;
    if (enj_state_startup() != 0) {
        printf("gfx_pvr: enDjinn could not start the video\n");
    }
    pvr_post_init();
}

// Everything set after pvr_init, again after a restart (gfx_pvr_restart).
static void pvr_post_init(void) {
#if DC_PT_CHUNK != 64
    // Punch-through chunk of the tile renderer: ISP_FEED_CFG (PVR register 0x098) bits 13..4, in
    // vertices. Per tile the ISP takes PT polygons this many vertices at a time and repeats its
    // passes per chunk. pvr_init writes 0x00800408 (chunk 64); only the chunk field changes.
#ifndef PVR_ISP_FEED_CFG
#define PVR_ISP_FEED_CFG PVR_UNK_0098   /* KOS before 315741eb named it by its address */
#endif
    PVR_SET(PVR_ISP_FEED_CFG, 0x00800008u | ((unsigned) DC_PT_CHUNK << 4));
#endif
#define PM_DITHER_BIT 8
    uint32_t cfg = PVR_GET(PVR_FB_CFG_2);
    cfg &= ~PM_DITHER_BIT;
    PVR_SET(PVR_FB_CFG_2, cfg);

    // PT alpha-test reference: punch-through discards texels with alpha <= this, giving
    // N64 cutout/texture-edge transparency. 0x80 matches OoT. (ARGB1555 alpha is 0/255,
    // so this cleanly drops the transparent bit; ARGB4444 keeps alpha >= ~8.)
    *(volatile uint32_t *) 0xA05F811C = 0x80 + 0x40;

    // Pre-first-frame default only — gfx_pvr_start_frame drives the real clear from the game's
    // clear_color global every frame (see sm_rgba5551_to_rgbf below).
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    // Headers are compiled lazily per depth/texture/list state at draw time.
    fade_hook_render_done();
}

static int sFsaaWant = 1;
#define FSAA_WANT sFsaaWant

// The player's choice (dc_video_options.h): off, on, or on except the scenes whose fill does not
// fit 60 fps with it. gfx_pvr_start_frame switches before the next scene begins.
int gDcFsaaMode = DC_FSAA_MODE;

static void gfx_pvr_request_fsaa_for_scene(int level, int area) {
    static const s16 kOff[] = { DC_FSAA_OFF_SCENES };
    if (gDcFsaaMode == DC_FSAA_MODE_TOGGLE_X || gDcFsaaMode == DC_FSAA_MODE_TOGGLE_B) {
        return;   // the button's choice, kept across scenes
    }
    sFsaaWant = gDcFsaaMode != DC_FSAA_MODE_OFF;
    if (gDcFsaaMode != DC_FSAA_MODE_CURATED) return;
    for (unsigned i = 0; i < sizeof(kOff) / sizeof(kOff[0]); i++) {
        if (kOff[i] == FSAA_SCENE(level, area)) sFsaaWant = 0;
    }
}

// area.c load_area: a course's first area, or another of its areas (a warp inside the course).
void gfx_pvr_area_loaded(void) {
    gfx_pvr_request_fsaa_for_scene(gCurrLevelNum, gCurrAreaIndex);
}

// The debug menu changed gDcFsaaMode (pc_main.c): the current scene's mode by it, switched before
// the next frame as at an area load.
void gfx_pvr_fsaa_mode_changed(void) {
    gfx_pvr_request_fsaa_for_scene(gCurrLevelNum, gCurrAreaIndex);
}

// The toggle button (DC_FSAA_MODE_TOGGLE_X/_B, controller_dc.c): the other mode, switched before
// the next frame as at an area load.
void gfx_pvr_fsaa_toggle(void) {
    sFsaaWant = !sFsaaWant;
    printf("FSAA: %s by the toggle button\n", sFsaaWant ? "on" : "off");
}

extern void reset_texcache(void);
extern void gfx_dc_set_screen(int width, int height, int fsaa);

// Restart the PVR in the wanted FSAA and video modes (the DKR/enDjinn recipe). pvr_shutdown resets
// the VRAM allocator, so every texture pointer is forgotten, never freed, and the front-end cache
// is wiped: the course's textures upload again on first use, in the new mode.
static void gfx_pvr_restart(void) {
    pvr_wait_ready();
    pvr_shutdown();
    sPvrParams.fsaa_enabled = FSAA_WANT;
    if (gDcVideo240p != sVideo240p) {
        vid_set_mode(pvr_video_mode(gDcVideo240p), PM_RGB888);
        enj_state_get()->video.display_mode = (vid_display_mode_generic_t) pvr_video_mode(gDcVideo240p);
    }
    pvr_fb_size(gDcVideo240p);
    pvr_init(&sPvrParams);
    enj_state_get()->video.pvr_params.fsaa_enabled = FSAA_WANT;   // enDjinn's copy of the parameters
    pvr_post_init();
    dc_overlay_pvr_reset();
    for (uint32_t i = 0; i < PVR_TEX_MAX; i++) {
        sTextures[i].addr = NULL;
        sTextures[i].alloc_size = 0;
    }
    sTexCount = 0;
    sBoundTex = 0;
    sCurBound = 0;
    reset_texcache();
    gfx_dc_set_screen(sFbW, sFbH, sPvrParams.fsaa_enabled);
    pvr_mark_dirty();
    printf("gfx_pvr: video %dx%d%s\n", sFbW, sFbH, sPvrParams.fsaa_enabled ? ", FSAA" : "");
}

// The FSAA or 240p option changed since the PVR started: restart it before the frame.
static inline int pvr_mode_stale(void) {
    return FSAA_WANT != sPvrParams.fsaa_enabled || gDcVideo240p != sVideo240p;
}

static void gfx_pvr_on_resize(void) { }

// The N64 scene clear color, an RGBA5551 value (low 16 bits) stashed by the game's
// clear_frame_buffer/clear_viewport (game_init.c) and geo_process_background. On this port those
// paths emit NO fillrect — they only set this global — so the BACKEND must consume it as its
// clear/background color. Without it every clear_frame_buffer scene shows the backend's hardcoded color.
extern int clear_color;

static inline void sm_rgba5551_to_rgbf(uint16_t c, float *r, float *g, float *b) {
    const float inv31 = 1.0f / 31.0f;
    *r = ((c >> 11) & 0x1F) * inv31;
    *g = ((c >>  6) & 0x1F) * inv31;
    *b = ((c >>  1) & 0x1F) * inv31;
}

static void gfx_pvr_start_frame(void) {
    float cr, cg, cb;
    // drive the PVR background plane from the game's clear_color (set last game-loop iteration).
    sm_rgba5551_to_rgbf((uint16_t) clear_color, &cr, &cg, &cb);
    pvr_set_bg_color(cr, cg, cb);
    // OP list open + live for the whole frame (guaranteed-opaque geometry only).
    // Anything with alpha (blend or alpha-test cutout) is deferred to the TR bucket.
    // enj_render_scene has opened it already (gfx_pvr_render_frame).
    pvr_dr_init(&sDrState);
    sCurrentList = PVR_LIST_OP_POLY;
    // 2D depth base: PVR z is 1/w, larger == nearer. The per-rect +0.0005 increment
    // (gfx_draw_rectangle) encodes 2D paint order into z, so the PT list resolves
    // 2D-among-2D layering correctly. Base 1.0 keeps those small increments well within
    // the depth buffer's resolution (a high base, e.g. 100, crushes them -> fill-rect
    // priority/ordering breaks). The black-rect issue this was once bumped for turned
    // out to be the 0x01200A00 shader, not depth, so 1.0 is the right base.
    screen_2d_z = 1.0f;

    // Disable the PVR near/far z-clip. The front-end already near-clips in software
    // (gfx_build_clipped_fan), and our far-plane background (the ortho skybox baked to
    // 1/w≈0.00001) sits below the default clip threshold and would otherwise be culled.
    pvr_set_zclip(0.0f);

    // Reset the deferred PT + TR buckets and force the live OP header to re-emit for the
    // new scene. sDepth*/sListKind persist in lockstep with the front-end's trackers.
    sTR.nverts = 0;
    sTR.nbatch = 0;
    sTR.cur = -1;
    sTR.dirty = 1;
    sPunch.nverts = 0;
    sPunch.nbatch = 0;
    sPunch.cur = -1;
    sPunch.dirty = 1;
    sOpDirty = 1;

    // Latch "did the last frame have a 3D (perspective) scene" so gfx_sp_tri1 can tell a
    // Z-off ortho BACKGROUND (over-skybox clouds, in a race) from a Z-off ortho OVERLAY
    // (menu glyph text). Prev-frame value avoids draw-order races within the frame.
    prev_frame_had_persp = cur_frame_persp;
    cur_frame_persp = 0;
    gfx_frame_invw_max = 0.0f;   // the frame's world 1/w bound (transition covers)
    has_done_3d = 0;   // reset: no 3D drawn yet this frame (title bg -> far until flag)
    fade_frame_start();
}

// enDjinn's deferred rendering (enj_render.c): the frame is queued as list renderers, and
// enj_render_scene waits for the PVR, begins the scene, opens each list that has renderers queued,
// calls them in order and finishes the scene. The display list runs in the OP list's renderer, so
// opaque geometry still streams live while it is translated; PT and TR fill their buckets then, and
// their renderers are queued from it only when there is something to send (a list without
// renderers is never opened). Native geometry (gfx_native.c) queues renderers of its own.
extern void enj_render_list_add(pvr_list_t renderlist, void (*renderer)(void *data), void *data);
extern void enj_render_scene(void);

static void pvr_render_pt(UNUSED void *data) {
    pvr_dr_init(&sDrState);
    pvr_flush_bucket(&sPunch);
}

static void pvr_render_tr(UNUSED void *data) {
    pvr_dr_init(&sDrState);
    pvr_flush_bucket(&sTR);
    dc_overlay_draw();   // last, over everything
}

static void (*sOpRenderer)(void *);
static void pvr_render_op(void *data) {
    sOpRenderer(data);
}

void gfx_pvr_render_frame(void (*op_renderer)(void *data), void *data) {
    if (pvr_mode_stale()) {
        gfx_pvr_restart();
    }
    sOpRenderer = op_renderer;
    enj_render_list_add(PVR_LIST_OP_POLY, pvr_render_op, data);
}

static void gfx_pvr_end_frame(void) {
    fade_registers();
    // The deferred buckets, each into its own list. PT before TR: alpha-test cutouts are opaque
    // pixels that write depth, so they must be down before TR composites over OP + PT.
    if (sPunch.nbatch > 0) {
        enj_render_list_add(PVR_LIST_PT_POLY, pvr_render_pt, NULL);
    }
    if (sTR.nbatch > 0 || DC_OVERLAY_SHOWN) {
        enj_render_list_add(PVR_LIST_TR_POLY, pvr_render_tr, NULL);
    }
}

// The x scale of the PVR's current mode: 2 with FSAA (twice as wide), else 1.
int gfx_pvr_fsaa_x(void) {
    return sPvrParams.fsaa_enabled ? 2 : 1;
}

static void gfx_pvr_finish_render(void) {
    // Nothing to do: enj_render_scene submitted the scene and flips.
}

struct GfxRenderingAPI gfx_pvr_api = {
    gfx_pvr_unload_shader,
    gfx_pvr_load_shader,
    gfx_pvr_create_and_load_new_shader,
    gfx_pvr_lookup_shader,
    gfx_pvr_shader_get_info,
    gfx_pvr_new_texture,
    gfx_pvr_select_texture,
    gfx_pvr_upload_texture,
    gfx_pvr_set_sampler_parameters,
    gfx_pvr_set_depth_test,
    gfx_pvr_set_depth_mask,
    gfx_pvr_set_zmode_decal,
    gfx_pvr_set_tex_env,
    gfx_pvr_set_viewport,
    gfx_pvr_set_scissor,
    gfx_pvr_set_use_alpha,
    gfx_pvr_draw_triangles,
    gfx_pvr_draw_quad_2d,
    gfx_pvr_init,
    gfx_pvr_on_resize,
    gfx_pvr_start_frame,
    gfx_pvr_end_frame,
    gfx_pvr_finish_render,
};
