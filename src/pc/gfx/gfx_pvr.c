// gfx_pvr.c — raw-KOS-PVR backend for the SM64 DC renderer (ported from MK64).
//
// Drop-in alternative to the GLdc backend (gfx_gldc.c): implements the same
// struct GfxRenderingAPI, but submits geometry straight to the PVR via the
// direct-render (store-queue) path instead of going through OpenGL. The
// gfx_retro_dc.c front-end (command interpreter, combiner, SW clip/cull/scissor)
// is shared; only this backend differs. SM64's rapi is the SINGLE-TEXTURE variant
// (shader_get_info returns the used-texture bool; no tile params), so this backend
// is adapted accordingly vs MK64's two-tile version.
//
// Selected at build time with `make GFX_BACKEND=pvr` (-DGFX_BACKEND_PVR), which
// also wires pc_main.c to pick gfx_pvr_api and tells gfx_dc.c to skip
// glKosSwapBuffers (the PVR flips inside pvr_scene_finish here).
//
// STAGE 1 (current): full backend machinery present + build-wired so the PVR scene
// lifecycle runs and the screen clears — proving the backend is live. gfx_pvr_draw_triangles
// is a NO-OP until the front-end PVR paths (clip + screen-coord bake + combiner eval) land in
// stage 3; SM64's GLdc front-end still emits object-space verts, which must not reach the TA.

// Whole file is PVR-backend-only. SM64's Makefile wildcards every src/pc/gfx/*.c, so this
// is compiled in the GLdc build too; the guard makes it an empty translation unit there
// (no gfx_pvr_api, no front-end coupling refs) so the default build links unchanged.
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

// The DR submit path relies on these: each vertex is copied dc_fast_t -> pvr_vertex_t
// (identical 32-byte layout), and the poly header is written into a single 32-byte
// store-queue slot. If a KOS revision changes either size, fail at build time rather
// than corrupting the SQ at runtime.
_Static_assert(sizeof(dc_fast_t) == sizeof(pvr_vertex_t), "dc_fast_t must match pvr_vertex_t layout");
_Static_assert(sizeof(pvr_poly_hdr_t) == 32, "pvr_poly_hdr_t must be one 32-byte SQ slot");

// ---------------------------------------------------------------------------
// Shader bookkeeping — identical bookkeeping to the GLdc backend. The front-end
// treats ShaderProgram opaquely (only via shader_get_info), so this layout is
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
    bool texture_used;   // SM64 is single-texture (matches gfx_gldc.c's ShaderProgram)
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
#define TR_MAX_BATCH     512

typedef struct { pvr_poly_hdr_t __attribute__((aligned(32))) hdr; uint32_t start, count; } PvrBatch;
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
static void pvr_compile_header(pvr_poly_hdr_t *out, int kind) {
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

// Append n vertices (n/3 tris) of already-screen-baked dc_fast_t to a bucket,
// starting a new batch when the bucket's header state has changed.
static void pvr_append(PvrBucket *b, int kind, const dc_fast_t *tris, size_t n) {
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
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

// Replay a bucket's batches (header + its verts) to the open list via the DR path.
static void pvr_flush_bucket(PvrBucket *b) {
    for (int i = 0; i < b->nbatch; i++) {
        PvrBatch *bt = &b->batch[i];
        if (!bt->count) continue;
        pvr_poly_hdr_t *hp = (pvr_poly_hdr_t *) pvr_dr_target(sDrState);
        *hp = bt->hdr;
        pvr_dr_commit(hp);
        for (uint32_t v = 0; v < bt->count; v++) {
            pvr_vertex_t *vp = (pvr_vertex_t *) pvr_dr_target(sDrState);
            *vp = b->verts[bt->start + v];
            // Flags are already set in the bucket: 3-vert tris by pvr_reserve, 4-vert quads by
            // gfx_pvr_draw_quad_2d. Do NOT re-stamp here — a blanket (v%3) would corrupt the 2D quads.
            pvr_dr_commit(vp);
        }
    }
}

// Scratch for POT padding (max 256x256 @ 16bpp = 128KB). MK64 textures are small.
static uint16_t sPadScratch[256 * 256] __attribute__((aligned(32)));

static inline uint32_t pvr_next_pot(uint32_t v) {
    v--; v |= v>>1; v |= v>>2; v |= v>>4; v |= v>>8; v |= v>>16; return v+1;
}
static inline int pvr_is_pot(uint32_t v) { return (v & (v - 1)) == 0; }

// Pad-copy with clamp-to-edge (mirrors GLdc's resample_tex): real image at top-left,
// pad columns/rows replicate the edge so bilinear at the border samples real texels.
static void pvr_pad16(const uint16_t *in, int iw, int ih, uint16_t *out, int ow, int oh) {
    int y;
    (void)oh;
    for (y = 0; y < ih; y++) {
        uint16_t *o = out + (y * ow);
        memcpy(o, in + (y * iw), iw * 2);
        uint16_t edge = (iw > 0) ? in[(y * iw) + iw - 1] : 0;
        for (int x = iw; x < ow; x++) o[x] = edge;
    }
    if (ih > 0) {
        const uint16_t *last = out + ((ih - 1) * ow);
        for (; y < oh; y++) memcpy(out + (y * ow), last, ow * 2);
    }
}

// Framebuffer dimensions — must match gfx_dc.c SCR_WIDTH/HEIGHT. LOWRES = native 240p (320x240).
#if LOWRES
#define DC_FB_W 320
#define DC_FB_H 240
#else
#define DC_FB_W 640
#define DC_FB_H 480
#endif

// Mirror of OoT's known-good params for this toolchain:
//   {OP, OP_MOD, TR, TR_MOD, PT} bin sizes, vtxbuf, dma, fsaa, autosort_disabled, overflow
// Live list is OP (guaranteed-opaque geometry). Alpha-TEST CUTOUTS (hard-edged fences/
// foliage/glyphs — texture_edge or texture-driven alpha) go to the deferred PT bucket;
// real alpha-BLEND surfaces go to the deferred TR bucket. At end_frame PT is flushed first
// (writes depth, alpha-tested opaque), then TR composites over it. All three lists' bins
// are live -> {OP, OP_MOD, TR, TR_MOD, PT}.
static pvr_init_params_t sPvrParams = {
    { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
    512 * 1024,
    1,  // dma
    0,  // fsaa
    0,  // autosort_disabled (0 = autosort on; needed for TR in P3)
    2,  // overflow buffers
    0,  // vbuf_doublebuf_disabled (0 = double-buffering on)
};

// ===========================================================================
// GfxRenderingAPI implementation
// ===========================================================================

static bool gfx_pvr_z_is_from_0_to_1(void) {
    // Matches the GLdc backend's value. PVR uses 1/w depth regardless.
    return false;
}

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
    sBoundTex = texture_id;
    sCurBound = texture_id;   // upload target follows the most recent bind (à la GLdc)
    pvr_mark_dirty();
}

// PVR_TXRFMT_ARGB1555 == 0x8366 (front-end's 1555 tag); anything else is 4444.
//#define PVR_TYPE_ARGB1555 0x8366

static void gfx_pvr_upload_texture(const uint8_t *buf16, int width, int height, unsigned int type) {
    struct PvrTex *t = &sTextures[sCurBound];
    // NON-twiddled on purpose: twiddling is a CPU reorder on every upload, and MK64
    // invalidates/re-uploads textures heavily — twiddling there caused a major slowdown
    // (GLdc stayed non-twiddled for the same reason). Trade-off: non-twiddled/stride
    // textures only point-sample, so bilinear-wanting textures (speedometer) look crusty.
    int fmt = type //((type == PVR_TXRFMT_ARGB1555) ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_ARGB4444)
              | PVR_TXRFMT_NONTWIDDLED;

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
    if (t->addr) pvr_txr_load((void *) src, t->addr, size);

    t->w = fw; t->h = fh; t->fmt = fmt;
    t->u_scale = us; t->v_scale = vs;
    pvr_mark_dirty();
}

static void gfx_pvr_set_sampler_parameters(bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct PvrTex *t = &sTextures[sBoundTex];
    // Match GLdc gfx_cm_to_opengl precedence: CLAMP wins, else MIRROR (mirror-repeat),
    // else plain repeat. PVR clamp == GL_CLAMP, PVR flip == GL_MIRRORED_REPEAT.
    t->filter = linear_filter ? 1 : 0;
    t->clampu = (cms & G_TX_CLAMP) ? 1 : 0;
    t->clampv = (cmt & G_TX_CLAMP) ? 1 : 0;
    t->flipu  = (!(cms & G_TX_CLAMP) && (cms & G_TX_MIRROR)) ? 1 : 0;
    t->flipv  = (!(cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR)) ? 1 : 0;
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
    PVR_SET(PVR_FOG_VERTEX_COLOR,
            PVR_PACK_COLOR(a * (1.0f / 255.0f), r * (1.0f / 255.0f),
                           g * (1.0f / 255.0f), b * (1.0f / 255.0f)));
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

// (Re)emit the live OP header to the open OP list when depth/texture state changed.
static inline void pvr_emit_op_header(void) {
    if (!sOpDirty) return;
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    pvr_compile_header(&hdr, PVR_KIND_OP);
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
        *v = *(const pvr_vertex_t *) &tris[i];
        v->flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pvr_dr_commit(v);
    }
}

// 2D quad depth counter owned by the front-end (gfx_draw_rectangle increments it).
// GLdc's start_frame reset it each frame; the PVR backend must too, or it grows
// unbounded. Positive base because PVR z is inverse-depth (negative would clip).
extern float screen_2d_z;
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
            *o = *(const pvr_vertex_t *) &q[i];
            o->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            pvr_dr_commit(o);
        }
        return;
    }

    PvrBucket *b = (sListKind == PVR_KIND_TR) ? &sTR : &sPunch;
    int kind     = (sListKind == PVR_KIND_TR) ? PVR_KIND_TR : PVR_KIND_PT;
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
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

static void gfx_pvr_init(void) {
#ifdef __DREAMCAST__
#if LOWRES
    // Native 240p (progressive). NTSC 320x240 is non-interlaced; VGA has a 320x240 mode too.
    vid_set_mode(vid_check_cable() != CT_VGA ? DM_320x240_NTSC : DM_320x240_VGA, PM_RGB565);
#else
    if (vid_check_cable() != CT_VGA)
        vid_set_mode(DM_640x480_NTSC_IL, PM_RGB565);
    else
        vid_set_mode(DM_640x480_VGA, PM_RGB565);
#endif
#endif
    pvr_init(&sPvrParams);
#if defined(__DREAMCAST__) && LOWRES
    PVR_SET(PVR_SCALER_CFG, 0x400);   // 240p: disable the vertical flicker/scanline filter
#endif

    // PT alpha-test reference: punch-through discards texels with alpha <= this, giving
    // N64 cutout/texture-edge transparency. 0x80 matches OoT. (ARGB1555 alpha is 0/255,
    // so this cleanly drops the transparent bit; ARGB4444 keeps alpha >= ~8.)
    *(volatile uint32_t *) 0xA05F811C = 0x80 + 0x40;

    // Pre-first-frame default only — gfx_pvr_start_frame drives the real clear from the game's
    // clear_color global every frame (see sm_rgba5551_to_rgbf below).
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    // Headers are compiled lazily per depth/texture/list state at draw time.
}

static void gfx_pvr_on_resize(void) { }

// The N64 scene clear color, an RGBA5551 value (low 16 bits) stashed by the game's
// clear_frame_buffer/clear_viewport (game_init.c) and geo_process_background. On this port those
// paths emit NO fillrect — they only set this global — so the BACKEND must consume it as its
// clear/background color (GLdc did this in gfx_opengl_start_frame). Without it every
// clear_frame_buffer scene shows the backend's hardcoded color. Matches gfx_gldc.c's decode.
extern int clear_color;
static inline void sm_rgba5551_to_rgbf(uint16_t c, float *r, float *g, float *b) {
    const float inv31 = 1.0f / 31.0f;
    *r = ((c >> 11) & 0x1F) * inv31;
    *g = ((c >>  6) & 0x1F) * inv31;
    *b = ((c >>  1) & 0x1F) * inv31;
}

static void gfx_pvr_start_frame(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    // Recreate GLdc's per-frame backend clear: drive the PVR background plane from the game's
    // clear_color (set last game-loop iteration, same one-frame cadence GLdc had).
    {
        float cr, cg, cb;
        sm_rgba5551_to_rgbf((uint16_t) clear_color, &cr, &cg, &cb);
        pvr_set_bg_color(cr, cg, cb);
    }
    // OP list open + live for the whole frame (guaranteed-opaque geometry only).
    // Anything with alpha (blend or alpha-test cutout) is deferred to the TR bucket.
    pvr_list_begin(PVR_LIST_OP_POLY);
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
    sTR.nverts    = 0; sTR.nbatch    = 0; sTR.cur    = -1; sTR.dirty    = 1;
    sPunch.nverts = 0; sPunch.nbatch = 0; sPunch.cur = -1; sPunch.dirty = 1;
    sOpDirty = 1;

    // Latch "did the last frame have a 3D (perspective) scene" so gfx_sp_tri1 can tell a
    // Z-off ortho BACKGROUND (over-skybox clouds, in a race) from a Z-off ortho OVERLAY
    // (menu glyph text). Prev-frame value avoids draw-order races within the frame.
    {
        extern int cur_frame_persp, prev_frame_had_persp, has_done_3d;
        prev_frame_had_persp = cur_frame_persp;
        cur_frame_persp = 0;
        has_done_3d = 0;   // reset: no 3D drawn yet this frame (title bg -> far until flag)
    }
}

static void gfx_pvr_end_frame(void) {
    pvr_list_finish();   // close the OP list (opened once this frame)

    // Flush the deferred buckets, each into its own list opened once. PT FIRST: alpha-test
    // cutouts are opaque pixels that write depth, so they must be down before TR composites.
    if (sPunch.nbatch > 0) {
        pvr_list_begin(PVR_LIST_PT_POLY);
        pvr_dr_init(&sDrState);
        pvr_flush_bucket(&sPunch);
        pvr_list_finish();
    }
    // TR SECOND: real alpha-blend surfaces, autosorted, composited over OP + PT (depth-tested).
    if (sTR.nbatch > 0) {
        pvr_list_begin(PVR_LIST_TR_POLY);
        pvr_dr_init(&sDrState);
        pvr_flush_bucket(&sTR);
        pvr_list_finish();
    }
}

static void gfx_pvr_finish_render(void) {
    // Submits the scene to the GPU and flips. (Replaces glKosSwapBuffers, which
    // gfx_dc.c skips under -DGFX_BACKEND_PVR.)
    pvr_scene_finish();
}

struct GfxRenderingAPI gfx_pvr_api = {
    gfx_pvr_z_is_from_0_to_1,
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
