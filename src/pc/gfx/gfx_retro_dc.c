#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "macros.h"

#include <stdio.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>
#include <GL/glkos.h>
#include "gl_fast_vert.h"

#include "sh4zam.h"

#ifndef GFX_BACKEND_PVR
#define GFX_BACKEND_PVR
#endif

int in_trilerp = 0;
int doing_peach = 0;
int doing_bowser = 0;

#define SUPPORT_CHECK(x) assert(x)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED (512)
#define MAX_LIGHTS 2
#define MAX_VERTICES 32

#undef bool
#define bool uint8_t
#define true 1
#define false 0

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used;
    int num_inputs;
};

// 4 bytes
struct RGBA {
    uint8_t r, g, b, a;
};

// 8 bytes
struct XYWidthHeight {
    // 0 2 4 6
    uint16_t x, y, width, height;
};

// exactly 32 bytes, 1 per cache line
struct __attribute__((aligned(32))) LoadedVertex {
    // 0, 4 — GLdc: NDC (x/w, y/w). PVR: RAW CLIP x, y (see gfx_sp_vertex; the homogeneous
    // SW clip + screen-bake in the PVR gfx_sp_tri1 path need raw clip coords, matching MK64).
    float _x, _y;
    // raw clip z, w — homogeneous clip planes, depth (1/w), and N64 fog (z/w). PVR-only so the
    // GLdc build keeps its lean one-cache-line vertex.
    float _z, _w;
    // 8, 12, 16
    float x, y, z;
    // 20, 24
    float u, v;
    // 28
    struct RGBA color;
    // 29
    // per-vertex N64 fog coefficient (0..255), computed at vertex-load from clip z/w and the RSP
    // fog mul/offset (G_FOG). Emitted into the PVR vertex's offset-colour (oargb) ALPHA, which
    // PVR hardware VERTEX fog reads as the fog density. (Ported from OoT/MK64.)
    uint8_t fog;
    // 30
    uint16_t pad;
};

// clip_rej for all 32 LoadedVerts fits in a single cache line
// bits 0 - 5 -> clip_rej
// bit 6 - wlt0
// bit 7 - lit
uint8_t __attribute__((aligned(32))) clip_rej[MAX_VERTICES];

struct TextureHashmapNode {
    // 0
    struct TextureHashmapNode *next;
    // 4
    const uint8_t *texture_addr;
    // 8
    uint32_t texture_id;
    // 12
    uint8_t cms, cmt;
    // 14
    uint8_t linear_filter;
    // 15
    uint8_t pad;
};

static struct {
    struct TextureHashmapNode __attribute__((aligned(32))) *hashmap[1024];
    struct TextureHashmapNode __attribute__((aligned(32))) pool[1024];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t shader_input_mapping[2][4];
};

static struct ColorCombiner color_combiner_pool[64];
static uint8_t color_combiner_pool_size;
#define MEM_BARRIER_PREF(ptr) asm volatile("pref @%0" : : "r"((ptr)) : "memory")
#define MEM_BARRIER() asm volatile("" : : : "memory");

static struct RSP {
    struct  __attribute__((aligned(32))) LoadedVertex loaded_vertices[MAX_VERTICES+4];
    struct __attribute__((aligned(32))) dc_fast_t loaded_vertices_2D[4];

    float MP_matrix[4][4] __attribute__((aligned(32)));
    float P_matrix[4][4] __attribute__((aligned(32)));
    float modelview_matrix_stack[11][4][4] __attribute__((aligned(32)));

    Light_t __attribute__((aligned(32))) current_lights[MAX_LIGHTS + 1];
    Light_t __attribute__((aligned(32))) current_lookat[2];
    float __attribute__((aligned(32))) current_lights_coeffs[MAX_LIGHTS][3];
    float __attribute__((aligned(32))) current_lookat_coeffs[2][3]; // lookat_x, lookat_y

    uint32_t __attribute__((aligned(32))) geometry_mode;
    uint16_t s_scale, t_scale;
    int16_t fog_mul, fog_offset;
    uint8_t modelview_matrix_stack_size;
    uint8_t current_num_lights; // includes ambient light
    uint8_t lights_changed;
    uint8_t use_fog;
    
} rsp __attribute__((aligned(32)));

static struct RDP {
    const uint8_t *palette;
    struct __attribute__((aligned(4))) {
        const uint8_t *addr;
        uint8_t siz;
    } texture_to_load;
    struct __attribute__((aligned(4))) {
        const uint8_t *addr;
        uint32_t size_bytes;
    } loaded_texture;
    struct __attribute__((aligned(4))) {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool texture_changed;
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    uint32_t combine_w0, combine_w1;   // raw G_SETCOMBINE words (PVR combiner evaluator)

    struct __attribute__((aligned(4))) RGBA env_color, prim_color, fog_color, fill_color;
    struct __attribute__((aligned(4))) XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp __attribute__((aligned(32)));

static struct RenderingState {
    // 0
    struct ShaderProgram *shader_program;
    // 4
    struct TextureHashmapNode *texture;
    // 5
    bool depth_test;
    // 6
    bool depth_mask;
    // 7
    bool decal_mode;
    // 8
    bool alpha_blend;
    // 9
    uint8_t fog_col_change;
    // 10: PVR texel<->colour combine (enum gfx_tex_env), derived from the N64 combiner
    uint8_t tex_env;
    // 11: PVR vertex fog on (G_FOG geometry mode)
    uint8_t fog_enabled;
    // 12 through 20, then to 31
    struct XYWidthHeight viewport, scissor;
} rendering_state __attribute__((aligned(32)));

struct GfxDimensions gfx_current_dimensions;

// Frame-context state shared with the raw-PVR backend (gfx_pvr.c). The backend's
// start_frame latches/resets these; the front-end's PVR draw paths (stages 3-4) populate
// them. Defined here in stage 1 so the backend links and boots to a clear screen.
//   screen_2d_z        : 2D paint-order depth counter (PVR z = 1/w; reset to 1.0/frame)
//   cur_frame_persp    : a perspective (3D) projection was used this frame
//   prev_frame_had_persp: cur_frame_persp from the previous frame
//   has_done_3d        : a 3D primitive has drawn this frame (background-vs-overlay ortho)
float screen_2d_z = 1.0f;
int cur_frame_persp = 0;
int prev_frame_had_persp = 0;
int has_done_3d = 0;
int proj_is_ortho = 0;   // current projection is orthographic (set in gfx_sp_matrix)
// Decal z-fight bias: a decal vert's z (=1/w) is scaled by this (>1 == nearer) so it wins the
// GEQUAL depth test against its coplanar base surface. Tune up if decals still z-fight, down if
// they punch through nearby geometry. (GLdc's equivalent magic numbers also needed tuning.)
#define PVR_DECAL_ZBIAS 1.003f

extern float gfx_pvr_get_u_scale(void);
extern float gfx_pvr_get_v_scale(void);

// Viewport (640x480 pixel space) -> screen-map. The raw-PVR backend does NO transform, so
// the front-end owns the full object->screen path: MVP at vertex-load (raw clip _x.._w),
// then perspective divide + viewport map here at emit:
//   screen_x = sm_xscale*(_x/_w) + sm_xbias
//   screen_y = sm_yscale*(_y/_w) + sm_ybias   (yscale<0: N64 NDC y-up -> PVR y-down)
//   screen_z = 1/_w                            (PVR inverse depth)
static float vpf_x = 0.0f, vpf_y = 0.0f, vpf_w = 640.0f, vpf_h = 480.0f;
static float sm_xscale = 320.0f, sm_xbias = 320.0f;
static float sm_yscale = -240.0f, sm_ybias = 240.0f;
static void gfx_recompute_screen_map(void) {
    float vw = vpf_w <= 0.0f ? 1.0f : vpf_w;
    float vh = vpf_h <= 0.0f ? 1.0f : vpf_h;
    float fb_h = (float) gfx_current_dimensions.height;
    sm_xscale = vw * 0.5f;
    sm_xbias  = vpf_x + vw * 0.5f;
    sm_yscale = -(vh * 0.5f);
    sm_ybias  = fb_h - vpf_y - vh * 0.5f;
}

// 3-way OP/PT/TR classifier + N64-blender->PVR factor derivation. The backend exposes the
// routing (gfx_pvr_set_blend) and TR factors (gfx_pvr_set_blend_factors); trackers here gate
// the flush-on-change in gfx_sp_tri1. Persist across frames in lockstep with the backend.
extern void gfx_pvr_set_blend(uint8_t kind);                    // 0=OP, 1=PT, 2=TR
extern void gfx_pvr_set_blend_factors(uint8_t src, uint8_t dst); // gfx_blend_factor codes
extern void gfx_pvr_set_fog(uint8_t enabled);                  // PVR vertex fog on/off (G_FOG)
extern void gfx_pvr_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// N64 per-vertex fog coefficient from clip z/w (matches OoT/MK64 raw-PVR + the RSP G_MWO_FOG
// math): fog = clamp(fog_mul*(z/w) + fog_offset, 0..255). 0 when fog is off (G_FOG clear) or the
// vertex is behind the near plane, so non-fogged frames pay nothing.
static inline uint8_t gfx_calc_fog(float z, float w) {
    if (!(rsp.geometry_mode & G_FOG)) return 0;
    if (z < -w) return 0;
    float f = (float) rsp.fog_mul * (z * shz_fast_invf(w)) + (float) rsp.fog_offset;
    if (f > 255.0f) f = 255.0f;
    else if (f < 0.0f) f = 0.0f;
    return (uint8_t) f;
}
static uint8_t pvr_cur_kind = 0;
static uint8_t pvr_cur_bsrc = GFX_BLENDF_SRCALPHA, pvr_cur_bdst = GFX_BLENDF_INVSRCALPHA;

// Derive PVR blend factors from the N64 blender (final cycle in other_mode_l). Standard
// "incoming over framebuffer" (P=CLR_IN, M=CLR_MEM) maps A->src factor, B->dst factor; any
// other arrangement falls back to plain alpha-over. (Ported from MK64 — game-agnostic.)
static void pvr_derive_blend(uint32_t oml, uint32_t omh, uint8_t *src, uint8_t *dst) {
    int base = ((omh & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) ? 16 : 24;
    int p = (oml >> (base + 6)) & 3, m = (oml >> (base + 4)) & 3;
    int a = (oml >> (base + 2)) & 3, b = (oml >> (base + 0)) & 3;
    if (p != G_BL_CLR_IN || m != G_BL_CLR_MEM) {
        *src = GFX_BLENDF_SRCALPHA; *dst = GFX_BLENDF_INVSRCALPHA;
        return;
    }
    *src = (a == G_BL_0) ? GFX_BLENDF_ZERO : GFX_BLENDF_SRCALPHA;
    *dst = (b == G_BL_1MA)   ? GFX_BLENDF_INVSRCALPHA
         : (b == G_BL_A_MEM) ? GFX_BLENDF_DSTALPHA
         : (b == G_BL_1)     ? GFX_BLENDF_ONE
         :                     GFX_BLENDF_ZERO;
}

// PVR streams ALL 3D geometry with NO buf_vbo: OP (kind 0) bakes into op_emit then DR-submits per
// source-triangle (pvr_submit_op); PT/TR (kind 1/2) bake DIRECTLY into their deferred bucket
// (pvr_reserve). op_emit holds ONE source triangle's near-clip fan = at most 2 tris = 6 verts
// (sm_near_clip_fan only clips the near plane; fan_tris[2][3]).
static dc_fast_t __attribute__((aligned(32))) op_emit[2 * 3];
extern void pvr_submit_op(const dc_fast_t *tris, size_t n);
extern dc_fast_t *pvr_reserve(int kind, size_t n);

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

int last_set_texture_image_width = 0;

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}
void n64_memcpy(void* dst, const void* src, size_t size);

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = (cc_id >> 24) << 24;
    uint8_t shader_input_mapping[2][4] = {{0}};
    int i, j;

    for (i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
    }
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id);
    n64_memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    size_t i;

    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }
    
    for (i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }

    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

void gfx_clear_texidx(unsigned int texidx);

void reset_texcache(void) {
    gfx_texture_cache.pool_pos = 0;
    memset(&gfx_texture_cache, 0, sizeof(gfx_texture_cache));
}
#if 0
static inline uint16_t hash10(uint32_t x) {
    // Knuth / golden-ratio-style multiplicative hash
    x *= 0x9E3779B1u; // 2654435761
    return (uint16_t)(x >> 22); // top 10 bits -> 0..1023
}
#endif
#if 0
static inline uint16_t hash10(uint32_t addr) {
    uint32_t x = addr >> 5;
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return (uint16_t) ((x >> 22) & 0x3ff);
}
#endif
#include <stdint.h>
#if 0
static inline uint32_t mix32(uint32_t x) {
    // MurmurHash3 32-bit finalizer (excellent avalanche)
    x ^= x >> 16;
    x *= 0x85ebca6bU;
    x ^= x >> 13;
    x *= 0xc2b2ae35U;
    x ^= x >> 16;
    return x;
}

static inline uint16_t hash10_addr_8c(uint32_t addr) {
    uint32_t key = (addr & 0x00FFFFFF) >> 5;//(addr >> 5) & ((1u << 19) - 1);   // drop low 5, ignore top 8
    uint32_t h   = mix32(key);
    return (uint16_t)(h >> 22);                      // 10-bit result
}
#endif
static inline uint16_t hash10_mul(uint32_t addr) {
    uint32_t key = (addr >> 5) & ((1u << 19) - 1);
    uint32_t h   = key * 0x9e3779b1U;   // golden ratio
    return (uint16_t)(h >> 22);
}
uint32_t cols_array[32];

static bool  __attribute__((noinline)) gfx_texture_cache_lookup(struct TextureHashmapNode **n, const uint8_t *orig_addr) {
    size_t hash = hash10_mul((uint32_t)orig_addr);//
    //((size_t)orig_addr >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
//    memset(cols_array, 0, sizeof(cols_array));
//    int colcount = 0;
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr) {
            gfx_rapi->select_texture((*node)->texture_id);
            *n = *node;
/*             if (colcount) {
                printf("lookup %08lx colcount %d\n", (uint32_t)orig_addr, colcount);
                for (int i=0;i<colcount;i++) {
                    printf("\t%08lx\n", cols_array[i]);
                }
            }
 */            return true;
        }/*  else {
            cols_array[colcount++] = (uint32_t)(*node)->texture_addr;
        } */
        node = &(*node)->next;
    }

    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];

    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }

    gfx_rapi->select_texture((*node)->texture_id);
    gfx_rapi->set_sampler_parameters(false, 0, 0);

    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;

    *n = *node;

    return false;
}

uint16_t __attribute__((aligned(32))) rgba16_buf[4096 * 2];

static void import_texture(void);

static void  __attribute__((noinline)) import_texture_rgba16(void) {
	uint32_t i;
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = rdp.loaded_texture.size_bytes / rdp.texture_tile.line_size_bytes;
    uint16_t *inbuf = (uint16_t*)rdp.loaded_texture.addr;

		for (i = 0; i < rdp.loaded_texture.size_bytes / 2; i++) {
			uint16_t col16 = inbuf[i];
            col16 = (col16 << 8) | ((col16 >> 8) & 0xff);
            rgba16_buf[i] = ((col16 & 1) << 15) | (col16 >> 1);
		}

		gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void  __attribute__((noinline)) import_texture_rgba32(void) {
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = (rdp.loaded_texture.size_bytes / 2) / rdp.texture_tile.line_size_bytes;

    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t r = (rdp.loaded_texture.addr[(4 * i) + 0] >> 4) & 0x0f;
        uint8_t g = (rdp.loaded_texture.addr[(4 * i) + 1] >> 4) & 0x0f;
        uint8_t b = (rdp.loaded_texture.addr[(4 * i) + 2] >> 4) & 0x0f;
        uint8_t a = (rdp.loaded_texture.addr[(4 * i) + 3] >> 4) & 0x0f;
        rgba16_buf[i] = (a << 12) | (r << 8) | (g << 4) | (b);
    }

    gfx_rapi->upload_texture((uint8_t *) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

uint8_t __attribute__((aligned(32))) xform_buf[8192];

static void  __attribute__((noinline)) import_texture_ia4(void) {
    uint32_t width = rdp.texture_tile.line_size_bytes << 1;
    uint32_t height = rdp.loaded_texture.size_bytes / rdp.texture_tile.line_size_bytes;
    uint32_t i;

    for (i = 0; i < rdp.loaded_texture.size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture.addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = (part & 0xE) << 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        uint16_t col16 = (alpha << 15) | (r << 10) | (g << 5) | (b);
        rgba16_buf[i] = col16;
    }

    gfx_rapi->upload_texture((uint8_t *) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void  __attribute__((noinline)) import_texture_ia8(void) {
    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture.size_bytes / rdp.texture_tile.line_size_bytes;

    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t val = rdp.loaded_texture.addr[i];
        uint8_t in = ((val >> 4) & 0xf);
        uint8_t al = (val & 0xf);
        rgba16_buf[i] = (al << 12) | (in << 8) | (in << 4) | in;
    }

    gfx_rapi->upload_texture((uint8_t *) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void  __attribute__((noinline)) import_texture_ia16(void) {
	uint32_t i;
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = rdp.loaded_texture.size_bytes / rdp.texture_tile.line_size_bytes;
	u32 src_width;
	if (last_set_texture_image_width) {
		src_width = last_set_texture_image_width + 1;
	} else {
		src_width = width;
	}

	uint16_t* start = (uint16_t *)rdp.loaded_texture.addr;
	if (last_set_texture_image_width) {
		start =
			(uint16_t*) &rdp.loaded_texture
				.addr[((((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC))) << 1) +
					  (((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC)) * ((src_width) << 1))];
	}

	uint16_t *tex16 = rgba16_buf;
	for (i = 0; i < height; i++) {
		for (uint32_t x = 0; x < src_width; x++) {
			uint16_t np = start[x];//++;
			uint8_t al = (np >> 12)&0xf;
			uint8_t in = (np >>  4)&0xf;
			tex16[x] = (al << 12) | (in << 8) | (in << 4) | in;
		}
		start += src_width;
		tex16 += src_width;
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, src_width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void  __attribute__((noinline)) import_texture_i4(void) {
	int width = rdp.texture_tile.line_size_bytes << 1;
	int height = rdp.loaded_texture.size_bytes / rdp.texture_tile.line_size_bytes;

	height = (height + 3) & ~3;

	if (last_set_texture_image_width == 0) {
		for (int i = 0; i < (int)rdp.loaded_texture.size_bytes; i++) {
			uint16_t idx = (i<<1);
			uint8_t byte = rdp.loaded_texture.addr[i];
			uint8_t part1,part2;
			part1 = (byte >> 4) & 0xf;
			part2 = byte & 0xf;
			rgba16_buf[idx  ] = (part1 << 12) | (part1 << 8) | (part1 << 4) | part1;
			rgba16_buf[idx+1] = (part2 << 12) | (part2 << 8) | (part2 << 4) | part2;
		}
	} else {
		memset(rgba16_buf, 0, 8192*sizeof(uint16_t));
		uint8_t* start =
			(uint8_t*) &rdp.loaded_texture
				.addr[(((((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC)-1)/2) * (width)/2)) +
					  (((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)-1)/2)];
		for (int i = 0; i < height; i++) {
			uint32_t iw = i * width;
			for (int x = 0; x < (last_set_texture_image_width + 1)*2; x += 2) {
				uint8_t startin = start[(x >> 1)];
				uint8_t in = (startin >> 4) & 0xf;
				rgba16_buf[iw + x] = (in << 12) | (in << 8) | (in << 4) | in;
				in = startin & 0xf;
				rgba16_buf[iw + x + 1] = (in << 12) | (in << 8) | (in << 4) | in;
			}
			start += (last_set_texture_image_width + 1);
		}
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void  __attribute__((noinline)) import_texture_i8(void) {
	uint32_t width = rdp.texture_tile.line_size_bytes;
	uint32_t height = rdp.loaded_texture.size_bytes / rdp.texture_tile.line_size_bytes;

	memset(xform_buf, 0, width*height*2);

	uint8_t* start = (uint8_t*) &rdp.loaded_texture
						 .addr[((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) * (last_set_texture_image_width + 1)) +
							   (rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)];

	u32 src_width;
	if (last_set_texture_image_width) {
		src_width = last_set_texture_image_width + 1;
	} else {
		src_width = width;
	}
	for (uint32_t i = 0; i < height; i++) {
		for (uint32_t x = 0; x < src_width; x++) {
			xform_buf[(i * width) + x] = start[x];
		}
		start += (src_width);
	}

	for (uint32_t i = 0; i < rdp.loaded_texture.size_bytes; i++) {
		uint8_t in = (xform_buf[i] >> 4) & 0xf;
		rgba16_buf[i] = (in << 12) | (in << 8) | (in << 4) | in;
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void  __attribute__((noinline)) import_texture(void) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
    
    if (rdp.loaded_texture.addr == NULL) {
        return;
    }

    if (gfx_texture_cache_lookup(&rendering_state.texture, rdp.loaded_texture.addr)) {
        return;
    }
    
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16();
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32();
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4();
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8();
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16();
        } else {
            abort();
        }
    }  else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4();
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8();
        } else {
            abort();
        }
    } else {
        abort();
    }
}

static void gfx_normalize_vector(float v[3]) {
    shz_vec3_t norm = shz_vec3_normalize((shz_vec3_t) { .x = v[0], .y = v[1], .z = v[2] });
    v[0] = norm.x;
    v[1] = norm.y;
    v[2] = norm.z;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    *((shz_vec3_t*) res) = shz_matrix4x4_trans_vec3_transpose((const shz_matrix_4x4_t *)b, *((shz_vec3_t*) a));
}
        #define recip127 0.00787402f

static void calculate_normal_dir(const Light_t* light, float coeffs[3]) {
    float light_dir[3] = { light->dir[0] * recip127, light->dir[1] * recip127, light->dir[2] * recip127 };
    gfx_transposed_matrix_mul(
        coeffs, light_dir,
        (const float (*)[4]) rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}


static void gfx_matrix_mul(shz_matrix_4x4_t* res, const shz_matrix_4x4_t* a, const shz_matrix_4x4_t* b) {
    shz_xmtrx_load_4x4_apply_store(res, b, a);
}

static int matrix_dirty = 0;

static void  __attribute__((noinline)) gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4] __attribute__((aligned(16)));
#ifndef GBI_FLOATS
    int i, j;
    // Original GBI where fixed point matrices are used
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    shz_xmtrx_load_4x4_unaligned((const float*)addr);
    shz_xmtrx_store_4x4((shz_matrix_4x4_t *)matrix);
#endif
    
    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            shz_xmtrx_store_4x4((shz_matrix_4x4_t *)rsp.P_matrix);
        } else {
            gfx_matrix_mul((shz_matrix_4x4_t *)rsp.P_matrix, (const shz_matrix_4x4_t *)matrix, (const shz_matrix_4x4_t *)rsp.P_matrix);
        }

        // Ortho vs perspective: the PERSPECTIVE term (the element that makes clip.w depend on z) is
        // [2][3] (column 3), NOT [3][2] (which is just the z-translation). Ortho: [3][3]~=1 and
        // [2][3]~=0; perspective: [3][3]~=0 and [2][3]~=-1. Using [3][2] here misclassified the
        // hand-rolled screen matrix `matrix_fullscreen` (z-translate [3][2]=-1, but ortho with
        // [2][3]=0) as perspective, so screen transitions / the cannon vignette never got the
        // foreground z2d band. Drives the 2D-overlay-vs-backdrop depth logic in gfx_sp_tri1.
        proj_is_ortho = (rsp.P_matrix[3][3] > 0.5f) && (rsp.P_matrix[2][3] > -0.5f);
        if (!proj_is_ortho) {
            cur_frame_persp = 1;
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            // why does this break goddard background
            // shz_matrix_4x4_copy((shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2]);
            n64_memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }

        if (parameters & G_MTX_LOAD) {
            shz_xmtrx_store_4x4((shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        } else {
            gfx_matrix_mul((shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)matrix, (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    matrix_dirty = 1;
    gfx_matrix_mul((shz_matrix_4x4_t *)rsp.MP_matrix, (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)rsp.P_matrix);
}

static void  __attribute__((noinline)) gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
        matrix_dirty = 1;
        gfx_matrix_mul((shz_matrix_4x4_t *)rsp.MP_matrix, (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)rsp.P_matrix);
    }
}

inline static uint8_t trivial_reject(float x, float y, float z, float w) {
    uint8_t cr = 0;

    if (z > w)
        cr |= 32;
    if (z < -w)
        cr |= 16;

    if (y > w)
        cr |= 8;
    if (y < -w)
        cr |= 4;

    if (x > w)
        cr |= 2;
    if (x < -w)
        cr |= 1;

    return cr;
}

#define MAX3(a, b, c) (MAX(MAX((a), (b)), (c)))
#define MAX4(a, b, c, d) (MAX(MAX3((a), (b), (c)), (d)))
#define MAX5(a, b, c, d, e) (MAX(MAX4((a), (b), (c), (d)), (e)))

static void  __attribute__((noinline)) gfx_sp_vertex_no(size_t n_vertices, size_t dest_index, const Vtx *vertices);
static void  __attribute__((noinline)) gfx_sp_vertex_light(size_t n_vertices, size_t dest_index, const Vtx *vertices);


static void  __attribute__((noinline)) gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    shz_xmtrx_load_4x4((const shz_matrix_4x4_t *)&rsp.MP_matrix);
    if (rsp.geometry_mode & G_LIGHTING) {
        if (rsp.lights_changed) {
            int i;
            for (i = 0; i < rsp.current_num_lights - 1; i++) {
                calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
            }
            calculate_normal_dir(&rsp.current_lookat[0], rsp.current_lookat_coeffs[0]);
            calculate_normal_dir(&rsp.current_lookat[1], rsp.current_lookat_coeffs[1]);
            rsp.lights_changed = false;
        }
        gfx_sp_vertex_light(n_vertices, dest_index, vertices);
    } else {
        gfx_sp_vertex_no(n_vertices, dest_index, vertices);
    }
}

static void __attribute__((noinline)) gfx_sp_vertex_light(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    size_t i;

    for (i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];

        shz_vec4_t out = shz_xmtrx_trans_vec4(shz_vec4_init((float)vn->ob[0], (float)vn->ob[1], (float)vn->ob[2], 1.0f));
        MEM_BARRIER_PREF(vn + 1);
        d->x = (float)vn->ob[0];
        d->y = (float)vn->ob[1];
        d->z = (float)vn->ob[2];

        short U = vn->tc[0] * rsp.s_scale >> 16;
        short V = vn->tc[1] * rsp.t_scale >> 16;
        d->color.a = vn->a;

        MEM_BARRIER();
        float x, y, z, w;
        x = out.x;
        y = out.y;
        z = out.z;
        w = out.w;
        MEM_BARRIER();

        int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
        int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
        int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
        int i;

        for (i = 0; i < rsp.current_num_lights - 1; i++) {
            float intensity = MAX((float) 0.0f, (float) (recip127 * shz_dot6f((float) vn->n[0], (float) vn->n[1], (float) vn->n[2], rsp.current_lights_coeffs[i][0], rsp.current_lights_coeffs[i][1], rsp.current_lights_coeffs[i][2])));

            if (intensity > 0.0f) {
                r += intensity * rsp.current_lights[i].col[0];
                g += intensity * rsp.current_lights[i].col[1];
                b += intensity * rsp.current_lights[i].col[2];
            }
        }

        d->color.r = MIN(255, r);
        d->color.g = MIN(255, g);
        d->color.b = MIN(255, b);

#define recip2pi 0.159155f

        if (rsp.geometry_mode & G_TEXTURE_GEN) {
            float dotx;
            float doty;
            {
                register float fr8 asm("fr8") = vn->n[0];
                register float fr9 asm("fr9") = vn->n[1];
                register float fr10 asm("fr10") = vn->n[2];
                register float fr11 asm("fr11") = 0;

                dotx = recip127 * shz_dot8f(fr8, fr9, fr10, fr11, rsp.current_lookat_coeffs[0][0], rsp.current_lookat_coeffs[0][1], rsp.current_lookat_coeffs[0][2], 0);

                doty = recip127 * shz_dot8f(fr8, fr9, fr10, fr11, rsp.current_lookat_coeffs[1][0], rsp.current_lookat_coeffs[1][1], rsp.current_lookat_coeffs[1][2], 0);
            }
            if (dotx < -1.0f)
                dotx = -1.0f;
            else if (dotx > 1.0f)
                dotx = 1.0f;

            if (doty < -1.0f)
                doty = -1.0f;
            else if (doty > 1.0f)
                doty = 1.0f;
            if (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) {
                dotx = shz_acosf(dotx) * recip2pi;
                doty = shz_acosf(doty) * recip2pi;
            } else {
                dotx = (dotx * 0.25f) + 0.25f;
                doty = (doty * 0.25f) + 0.25f;
            }

            U = (int32_t) (dotx * rsp.s_scale);
            V = (int32_t) (doty * rsp.t_scale);
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        uint8_t cr = 128 | ((w < 0) ? 64 : 0x00);
        clip_rej[dest_index] = cr | trivial_reject(x, y, z, w);

        // raw CLIP coords (no perspective divide) — the PVR gfx_sp_tri1 path clips
        // homogeneously then bakes screen_x/y + 1/w from these (stage 3).
        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;
        d->fog = gfx_calc_fog(z, w);   // per-vertex N64 fog coefficient -> oargb.alpha at emit
    }
}

static void  __attribute__((noinline)) gfx_sp_vertex_no(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    size_t i;

	for (i = 0; i < n_vertices; i++, dest_index++) {
		const Vtx_t* v = &vertices[i].v;
		struct LoadedVertex* d = &rsp.loaded_vertices[dest_index];

        shz_vec4_t out = shz_xmtrx_trans_vec4(shz_vec4_init((float)v->ob[0], (float)v->ob[1], (float)v->ob[2], 1.0f));
        MEM_BARRIER_PREF(v + 1);
        d->x = (float)v->ob[0];
        d->y = (float)v->ob[1];
        d->z = (float)v->ob[2];

        short U = v->tc[0] * rsp.s_scale >> 16;
        short V = v->tc[1] * rsp.t_scale >> 16;
        d->color.a = v->cn[3];

        MEM_BARRIER();
        float x, y, z, w;
        x = out.x;
        y = out.y;
        z = out.z;
        w = out.w;
        MEM_BARRIER();

        d->color.r = v->cn[0];
        d->color.g = v->cn[1];
        d->color.b = v->cn[2];

        d->u = U;
        d->v = V;

        // trivial clip rejection
        uint8_t cr = ((w < 0) ? 64 : 0x00);
        clip_rej[dest_index] = cr | trivial_reject(x, y, z, w);

        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;   // raw clip (see gfx_sp_vertex_light)
        d->fog = gfx_calc_fog(z, w);
    }
}

//
// utility variables and functions for implementing the peach/bowser trilerp effect
//

uint8_t trilerp_a = 0;

extern void get_mario_pos(float *x, float *y, float *z);

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static inline float step_ramp_pow(float x, float param, float n) {
    if (param >= 1.0f) return 0.0f;          // degenerate: never ramps
    if (param <= 0.0f) param = 0.0f;

    float t = clamp01((x - param) / (1.0f - param));
    return powf(t, n);
}

extern u8 gWarpTransRed;
extern u8 gWarpTransGreen;
extern u8 gWarpTransBlue;

// ---- Raw-PVR colour combiner evaluator (raw N64 mux, ported from MK64) -------------
// Evaluate cycle-0 (a-b)*c+d for colour AND alpha from the RAW G_SETCOMBINE words. TEXEL
// substitutes to 1.0 (the PVR does the real texture modulate), so argb is the modulate
// colour; a constant additive 'd' / a texture-independent colour is routed to oargb (added
// post-modulate via the always-on specular bit). Carries MK64's cloud-oargb + has_texel
// d-slot fixes. (a/b/d 4-bit, c 5-bit; mux 6 == G_CCMUX_1 == 1.0.)
static inline float pvr_cc4(int mux, int ch, float tex, const float prim[4], const float env[4], const float shade[4]) {
    switch (mux) {
        case 1: case 2: return tex;   // TEXEL0 / TEXEL1 placeholder (substituted per texenv mode)
        case 3: return prim[ch];
        case 4: return shade[ch];
        case 5: return env[ch];
        case 6: return 1.0f;
        default: return 0.0f;
    }
}
static inline float pvr_cc5(int mux, int ch, float tex, const float prim[4], const float env[4], const float shade[4]) {
    switch (mux) {
        case 1: case 2: case 8: case 9: return tex;   // TEXEL0/1 colour or alpha placeholder
        case 3: return prim[ch];
        case 4: return shade[ch];
        case 5: return env[ch];
        case 6: return 1.0f;
        case 10: return prim[3];
        case 11: return shade[3];
        case 12: return env[3];
        default: return 0.0f;
    }
}
static inline float pvr_ca(int mux, const float prim[4], const float env[4], const float shade[4]) {
    switch (mux) {
        case 1: case 2: return 1.0f;
        case 3: return prim[3];
        case 4: return shade[3];
        case 5: return env[3];
        case 6: return 1.0f;
        default: return 0.0f;
    }
}

// Derive the PVR texenv mode from the N64 1-cycle combiner (compact cc_id = rdp.combine_mode, the
// CC_ enum). Maps the combiner's texel<->colour relationship onto REPLACE/MODULATE/DECAL/
// MODULATEALPHA — replaces GLdc's per-shader-id texenv hacks. See [[reference_pvr_texenv_derivation]].
// 2-cycle is left on MODULATE for now (scope: 1-cycle first).
static uint32_t derive_pvr_texenv(uint32_t cc_id) {
    int ca = (cc_id >> 0) & 7, cb = (cc_id >> 3) & 7, cc = (cc_id >> 6) & 7, cd = (cc_id >> 9) & 7;
    int aa = (cc_id >> 12) & 7, ab = (cc_id >> 15) & 7, ac = (cc_id >> 18) & 7, ad = (cc_id >> 21) & 7;
    #define CC_ISTEX(v)  ((v) == CC_TEXEL0 || (v) == CC_TEXEL1)
    #define CC_ISTEXA(v) ((v) == CC_TEXEL0A)
    int color_has_tex = CC_ISTEX(ca) || CC_ISTEX(cb) || CC_ISTEX(cc) || CC_ISTEX(cd) ||
                        CC_ISTEXA(ca) || CC_ISTEXA(cb) || CC_ISTEXA(cc) || CC_ISTEXA(cd);
    if (!color_has_tex) return GFX_TEXENV_MODULATE;     // texel-free colour: MODULATE + oargb routing

    int color_single = (cc == CC_0) || (ca == cb);      // colour = cd
    int color_mul    = (cb == CC_0) && (cd == CC_0);    // colour = ca * cc
    int color_mix    = !color_single && (cb == cd);     // colour = lerp(cd, ca, cc)
    int alpha_single_tex = ((ac == CC_0) || (aa == ab)) && CC_ISTEX(ad);   // alpha = ad, a texel
    int alpha_uses_texel = CC_ISTEX(aa) || CC_ISTEX(ab) || CC_ISTEX(ac) || CC_ISTEX(ad);

    uint32_t mode = GFX_TEXENV_MODULATE;
    if (color_mix && CC_ISTEX(ca) && CC_ISTEXA(cc)) {
        mode = GFX_TEXENV_DECAL;                         // lerp(base, TEXEL0, TEXEL0A) — the Boo, BLEND*
    } else if (color_single && CC_ISTEX(cd)) {
        // colour = TEXEL0; the ALPHA mux picks the mode:
        //   alpha = TEXEL0          -> REPLACE        (DECALRGBA: rgb=tex, a=tex.a)
        //   alpha = TEXEL0 * const  -> MODULATEALPHA  (DECALFADEA: a = tex.a*env.a). col.rgb evaluates
        //          to white for a single-texel colour, so MODULATEALPHA's rgb=col*tex == tex while the
        //          texel alpha is preserved. DECAL here would force a=col.a (const) and drop tex.a,
        //          turning a transparent surround into a semi-opaque dark box (HMC wall flames).
        //   alpha = const, no texel -> DECAL          (DECALRGB a=shade, DECALFADE a=env)
        mode = alpha_single_tex ? GFX_TEXENV_REPLACE
             : alpha_uses_texel  ? GFX_TEXENV_MODULATEALPHA
                                 : GFX_TEXENV_DECAL;
    } else if (color_mul && (CC_ISTEX(ca) || CC_ISTEX(cc))) {
        mode = alpha_single_tex ? GFX_TEXENV_MODULATE        // colour=TEXEL0*col, alpha=TEXEL0
                                : GFX_TEXENV_MODULATEALPHA;  // colour=TEXEL0*col, alpha=col(*tex)
    }
    #undef CC_ISTEX
    #undef CC_ISTEXA
    return mode;
}

static inline void pvr_eval_combiner(uint32_t w0, uint32_t w1, const struct RGBA* sh,
                                     int textured, uint32_t mode, uint32_t* out_argb, uint32_t* out_oargb) {
    // REPLACE ignores the vertex colour (px = texel); oargb (added afterward) must be 0.
    if (mode == GFX_TEXENV_REPLACE) { *out_argb = 0xFFFFFFFFu; *out_oargb = 0; return; }

    const float r255 = 1.0f / 255.0f;
    const float prim[4]  = { rdp.prim_color.r*r255, rdp.prim_color.g*r255, rdp.prim_color.b*r255, rdp.prim_color.a*r255 };
    const float env[4]   = { rdp.env_color.r*r255,  rdp.env_color.g*r255,  rdp.env_color.b*r255,  rdp.env_color.a*r255 };
    const float shade[4] = { sh->r*r255, sh->g*r255, sh->b*r255, sh->a*r255 };
    // DECAL: PVR lerps vtx<->texel by texel.a, so the FACE colour is the combiner with the texel
    // terms ABSENT (texel->0; e.g. Boo's (TEXEL0-SHADE)*TEXEL0A+SHADE collapses to SHADE).
    // MODULATE/MODULATEALPHA factor the texel OUT (texel->1) to get the multiplier colour.
    const float texv = (mode == GFX_TEXENV_DECAL) ? 0.0f : 1.0f;

    int a = (w0 >> 20) & 0xF, b = (w1 >> 28) & 0xF, c = (w0 >> 15) & 0x1F, d = (w1 >> 15) & 0x7;
    int aa = (w0 >> 12) & 0x7, ab = (w1 >> 12) & 0x7, ac = (w0 >> 9) & 0x7, ad = (w1 >> 9) & 0x7;

    // colour samples the texture? check ALL four slots incl. d (G_CC_DECALRGB = (0,0,0,TEXEL0)).
    int has_texel = (a == 1 || a == 2 || b == 1 || b == 2 ||
                     c == 1 || c == 2 || c == 8 || c == 9 ||
                     d == 1 || d == 2);
    // colour-from-constant on a textured (alpha-only) poly -> route whole colour to oargb so
    // MODULATE doesn't darken it. Only trustworthy in 1-cycle (cycle-1 texel is invisible here).
    int two_cycle = ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE);
    int color_const_textured = textured && !has_texel && !two_cycle;
    // The combiner's additive d term ((a-b)*c + d) can't be done in PVR's MODULATE (which only
    // multiplies texel*col), so route a constant/per-vertex d into oargb (added post-modulate):
    // d == PRIM(3) / SHADE(4) / ENV(5). This is what makes "(X)*texel + d" combiners correct —
    // REFLECTRGB (env*tex+shade), HILITERGB ((prim-shade)*tex+shade, the goddard head), BLENDI,
    // ADDRGB — instead of collapsing to (X+d)*tex (dark everywhere, only bright texels survive).
    // DECAL keeps its additive base colour in argb (via texv=0), so don't also route it to oargb.
    int offset = (mode != GFX_TEXENV_DECAL) && !color_const_textured &&
                 (d == 3 || d == 4 || d == 5) && has_texel;

    uint32_t col[4];
    uint32_t oarr[3] = { 0, 0, 0 };
    for (int ch = 0; ch < 3; ch++) {
        float va = pvr_cc4(a, ch, texv, prim, env, shade);
        float vb = pvr_cc4(b, ch, texv, prim, env, shade);
        float vc = pvr_cc5(c, ch, texv, prim, env, shade);
        if (color_const_textured) {
            float v = (va - vb) * vc + pvr_cc4(d, ch, texv, prim, env, shade);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            oarr[ch] = (uint32_t)(v * 255.0f);
            col[ch]  = 0;
        } else {
            float vd = offset ? 0.0f : pvr_cc4(d, ch, texv, prim, env, shade);
            float v = (va - vb) * vc + vd;
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            col[ch] = (uint32_t)(v * 255.0f);
        }
    }
    if (color_const_textured) {
        *out_oargb = PACK_ARGB8888(oarr[0], oarr[1], oarr[2], 0);
    } else if (offset) {
        const float* o = (d == 3) ? prim : (d == 4) ? shade : env;
        *out_oargb = PACK_ARGB8888((uint32_t)(o[0]*255.0f), (uint32_t)(o[1]*255.0f), (uint32_t)(o[2]*255.0f), 0);
    } else {
        *out_oargb = 0;
    }
    float av = (pvr_ca(aa, prim, env, shade) - pvr_ca(ab, prim, env, shade)) * pvr_ca(ac, prim, env, shade)
               + pvr_ca(ad, prim, env, shade);
    av = av < 0.0f ? 0.0f : (av > 1.0f ? 1.0f : av);
    col[3] = (uint32_t)(av * 255.0f);
    *out_argb = PACK_ARGB8888(col[0], col[1], col[2], col[3]);
}

// ---- Near-plane (eye) clip ---------------------------------------------------------------
// The raw-PVR path bakes screen_x/y + 1/w per vertex, so a triangle straddling the near plane
// must be CLIPPED, not dropped (dropping = geometry vanishes near the camera). Clip against the
// NEAR plane z+w>=0 (z >= -w, the same plane trivial_reject uses) — NOT the eye plane w~0:
// near-plane intersections land at a FINITE projected position, whereas eye-plane (w~0)
// intersections project to ~infinity (1/w huge) and explode the triangle across the screen.
// Sutherland-Hodgman; interpolates only the fields the PVR emit reads (clip _x.._w, UV, colour).
// One plane turns a tri into 3 or 4 verts -> 1 or 2 output tris.
static inline float sm_near_dist(const struct LoadedVertex *v) { return v->_z + v->_w; }
static struct LoadedVertex sm_clipv[4] __attribute__((aligned(32)));
static inline void sm_clip_lerp(struct LoadedVertex *o, const struct LoadedVertex *a,
                                const struct LoadedVertex *b, float t) {
    o->_x = a->_x + t * (b->_x - a->_x);
    o->_y = a->_y + t * (b->_y - a->_y);
    o->_z = a->_z + t * (b->_z - a->_z);
    o->_w = a->_w + t * (b->_w - a->_w);
    o->u  = a->u  + t * (b->u  - a->u);
    o->v  = a->v  + t * (b->v  - a->v);
    o->color.r = (uint8_t)(a->color.r + t * ((float)b->color.r - (float)a->color.r));
    o->color.g = (uint8_t)(a->color.g + t * ((float)b->color.g - (float)a->color.g));
    o->color.b = (uint8_t)(a->color.b + t * ((float)b->color.b - (float)a->color.b));
    o->color.a = (uint8_t)(a->color.a + t * ((float)b->color.a - (float)a->color.a));
    o->fog     = (uint8_t)(a->fog     + t * ((float)b->fog     - (float)a->fog));
}
// Returns the output triangle count (0..2); out[ti] is a 3-pointer triangle into the originals
// or into sm_clipv (valid until the next call — emit immediately).
static int sm_near_clip_fan(struct LoadedVertex *v1, struct LoadedVertex *v2,
                            struct LoadedVertex *v3, struct LoadedVertex *out[2][3]) {
    float d1 = sm_near_dist(v1), d2 = sm_near_dist(v2), d3 = sm_near_dist(v3);
    if (d1 >= 0.0f && d2 >= 0.0f && d3 >= 0.0f) {   // fully in front of the near plane
        out[0][0] = v1; out[0][1] = v2; out[0][2] = v3;
        return 1;
    }
    struct LoadedVertex *in[3] = { v1, v2, v3 };
    float din[3] = { d1, d2, d3 };
    int n = 0;
    for (int i = 0; i < 3; i++) {
        struct LoadedVertex *cur = in[i];
        struct LoadedVertex *nxt = in[(i + 1 == 3) ? 0 : (i + 1)];
        float dc = din[i], dn = din[(i + 1 == 3) ? 0 : (i + 1)];
        int inc = dc >= 0.0f, inn = dn >= 0.0f;
        if (inc && n < 4) sm_clipv[n++] = *cur;
        if (inc != inn && n < 4) {
            float t = dc / (dc - dn);
            sm_clip_lerp(&sm_clipv[n++], cur, nxt, t);
        }
    }
    if (n < 3) return 0;
    out[0][0] = &sm_clipv[0]; out[0][1] = &sm_clipv[1]; out[0][2] = &sm_clipv[2];
    if (n == 4) {
        out[1][0] = &sm_clipv[0]; out[1][1] = &sm_clipv[2]; out[1][2] = &sm_clipv[3];
        return 2;
    }
    return 1;
}

static void  __attribute__((noinline)) gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    struct LoadedVertex* v1 = &rsp.loaded_vertices[vtx3_idx];
    MEM_BARRIER_PREF(v1);
    struct LoadedVertex* v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &rsp.loaded_vertices[vtx1_idx];
    uint8_t l_clip_rej[3] = { clip_rej[vtx3_idx], clip_rej[vtx2_idx], clip_rej[vtx1_idx] };
    MEM_BARRIER_PREF(v2);
    struct LoadedVertex* v_arr[3] = { v1, v2, v3 };
    uint8_t c0 = l_clip_rej[0];
    uint8_t c1 = l_clip_rej[1];
    uint8_t c2 = l_clip_rej[2];
    MEM_BARRIER_PREF(v3);

    if ((c0 & c1 & c2) & 0x3f) {
        // The whole triangle lies outside the visible area
        return;
    }
    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        // PVR stores RAW CLIP _x,_y; the back-face test needs NDC, so divide per vertex.
        float rw1 = shz_fast_invf(v1->_w), rw2 = shz_fast_invf(v2->_w), rw3 = shz_fast_invf(v3->_w);
        float dx1 = (v1->_x * rw1) - (v2->_x * rw2);
        float dy1 = (v1->_y * rw1) - (v2->_y * rw2);
        float dx2 = (v3->_x * rw3) - (v2->_x * rw2);
        float dy2 = (v3->_y * rw3) - (v2->_y * rw2);
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((c0 ^ c1 ^ c2) & 0x40) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross >= 0) {
                    return;
                }
                break;
            case G_CULL_BACK:
                if (cross <= 0) {
                    return;
                }
                break;
            default:
                break;
        }
    }

    if (matrix_dirty) {
        matrix_dirty = 0;
    }

    if (in_trilerp) {
        if (doing_bowser) {
            float mx,my,mz;
            get_mario_pos(&mx,&my,&mz);
            float distance_frac = -(mz * 0.00025f);

            if (distance_frac < 0.0f)
			 	distance_frac = 0.0f;
            else if (distance_frac > 1.0f)
    			distance_frac = 1.0f;

            distance_frac = 1.0f - step_ramp_pow(distance_frac, 0.8f, 2.0f);

            trilerp_a = (uint8_t)(distance_frac * 255.0f);
        }
    }

    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    // Per-surface depth TEST follows the render mode's Z_CMP bit (the "ZB" in G_RM_AA_ZB_*), not
    // just the global G_ZBUFFER geometry mode (which is set almost everywhere). This mirrors how
    // depth WRITE already reads Z_UPD below. Non-ZB render modes — screen transitions, the cannon
    // vignette, 2D overlays, paint-order effects — clear Z_CMP, i.e. the hardware itself says
    // "don't depth-test me". They then passively fall into the ortho_overlay foreground path
    // (frontmost z2d) instead of z-rejecting behind 3D. No DL-flag sniffing needed.
    depth_test = depth_test && (rdp.other_mode_l & Z_CMP);

    if (depth_test != rendering_state.depth_test) {
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
//            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            n64_memcpy(&rendering_state.viewport, &rdp.viewport, sizeof(rdp.viewport));
            // PVR backend's set_viewport is a no-op; the front-end owns the screen map.
            vpf_x = rdp.viewport.x; vpf_y = rdp.viewport.y;
            vpf_w = rdp.viewport.width; vpf_h = rdp.viewport.height;
            gfx_recompute_screen_map();
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            n64_memcpy(&rendering_state.scissor, &rdp.scissor, sizeof(rdp.scissor));
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    uint32_t cc_id = rdp.combine_mode;
    
    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    if ((rsp.use_fog != use_fog)) {
        rsp.use_fog = use_fog;
    }
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    
    if (texture_edge) {
        use_alpha = true;
    }
    
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    
    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }
    
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }

    if (use_alpha != rendering_state.alpha_blend) {
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }

    uint8_t num_inputs;
    uint8_t usetex = gfx_rapi->shader_get_info(prg, &num_inputs);
    int i;

    uint8_t linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;

    if (usetex) {
        if (rdp.texture_changed) {
            import_texture();
            rdp.texture_changed = 0;
        }
        uint8_t cms = rdp.texture_tile.cms;
        uint8_t cmt = rdp.texture_tile.cmt;

        uint32_t tex_size_bytes = rdp.loaded_texture.size_bytes;
        uint32_t line_size = rdp.texture_tile.line_size_bytes;
        uint32_t tex_height_i;
        if (line_size == 0) {
            line_size = 1;
            tex_height_i = tex_size_bytes;
        } else {
            tex_height_i = (uint32_t) shz_divf((float) tex_size_bytes , (float) line_size);
        }

        switch (rdp.texture_tile.siz) {
            case G_IM_SIZ_4b:
                line_size <<= 1;
                break;
            case G_IM_SIZ_8b:
                break;
            case G_IM_SIZ_16b:
                line_size >>= 1;
                break;
            case G_IM_SIZ_32b:
                line_size >>= 1;
                tex_height_i >>= 1;
                break;
        }
        uint32_t tex_width_i = line_size;

        uint32_t tex_width2_i = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) >> 2;
        uint32_t tex_height2_i = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) >> 2;

        uint32_t tex_width1 = tex_width_i << (cms & G_TX_MIRROR);
        uint32_t tex_height1 = tex_height_i << (cmt & G_TX_MIRROR);

        if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || (tex_width1 != tex_width2_i))) {
            cms &= (~G_TX_CLAMP);
        }

        if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || (tex_height1 != tex_height2_i))) {
            cmt &= (~G_TX_CLAMP);
        }

        linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
        gfx_rapi->set_sampler_parameters(linear_filter, cms, cmt);
        rendering_state.texture->linear_filter = linear_filter;
        rendering_state.texture->cms = cms;
        rendering_state.texture->cmt = cmt;
    }

    float recip_tw = 0.03125f;
    float recip_th = 0.03125f;
    if (usetex) {
        uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) >> 2;
        uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) >> 2;

        recip_tw = shz_fast_invf(tex_width);
        recip_th = shz_fast_invf(tex_height);
    }

    // ---- 3-way OP/PT/TR classification (raw alpha mux + render-mode flags) --------------
    //   FORCE_BL set            -> TR (real alpha blend, autosorted, composited last)
    //   else alpha-test CUTOUT  -> PT (coverage edge OR texel-alpha WITH alpha compare on)
    //   else                    -> OP (opaque, live)
    // (The GLdc packedc cascade above is #ifndef'd out on PVR — pvr_eval_combiner replaces it in
    //  the emit loop, so there's nothing to compute or silence here.)
    {
        int aa = (rdp.combine_w0 >> 12) & 7, ab = (rdp.combine_w1 >> 12) & 7;
        int ac = (rdp.combine_w0 >>  9) & 7, ad = (rdp.combine_w1 >>  9) & 7;
        uint8_t alpha_uses_texel = (aa == 1 || aa == 2 || ab == 1 || ab == 2 ||
                                    ac == 1 || ac == 2 || ad == 1 || ad == 2);
        uint8_t alpha_compare_on = (rdp.other_mode_l & 3) != 0;   // G_AC_THRESHOLD/DITHER
        uint8_t real_blend = (rdp.other_mode_l & FORCE_BL) != 0;
        uint8_t cutout     = texture_edge || (alpha_uses_texel && alpha_compare_on);
        uint8_t kind = real_blend ? 2 : (cutout ? 1 : 0);   // TR : PT : OP
        if (kind != pvr_cur_kind) {
            gfx_pvr_set_blend(kind);
            pvr_cur_kind = kind;
        }
        if (kind == 2) {   // TR: derive + push blend factors (flush on change)
            uint8_t bs, bd;
            pvr_derive_blend(rdp.other_mode_l, rdp.other_mode_h, &bs, &bd);
            if (bs != pvr_cur_bsrc || bd != pvr_cur_bdst) {
                gfx_pvr_set_blend_factors(bs, bd);
                pvr_cur_bsrc = bs; pvr_cur_bdst = bd;
            }
        }
    }

    // Near-plane clip the triangle into a fan of 1-2 tris (instead of dropping eye-crossers).
    struct LoadedVertex *fan_tris[2][3];
    int n_tris = sm_near_clip_fan(v1, v2, v3, fan_tris);
    if (n_tris == 0) return;   // fully behind the eye plane
    // Texenv derived from the N64 combiner (REPLACE/MODULATE/DECAL/MODULATEALPHA). A change ends
    // the current batch — the PVR poly header (cxt.txr.env) is per-batch — so flush first, then
    // the per-vertex argb below is evaluated in the matching mode.
    uint32_t texenv = usetex ? derive_pvr_texenv(rdp.combine_mode) : GFX_TEXENV_MODULATE;
    if (texenv != rendering_state.tex_env) {
        gfx_rapi->set_tex_env(texenv);
        rendering_state.tex_env = texenv;
    }
    // PVR hardware vertex fog: on when the RSP G_FOG geometry mode is set (the same signal that
    // makes gfx_calc_fog produce per-vertex coefficients). fog_type is in the poly header, so a
    // change ends the batch. (The fog-COLOUR register is a single global per frame, set in
    // gfx_dp_set_fog_color under the FIRST-WINS fog_col_change guard — NOT per draw.)
    uint8_t fog_on = (rsp.geometry_mode & G_FOG) != 0;
    if (fog_on != rendering_state.fog_enabled) {
        gfx_pvr_set_fog(fog_on);
        rendering_state.fog_enabled = fog_on;
    }
    // PVR has no buf_vbo — the deferred bucket guards its own overflow (pvr_reserve returns NULL).
    // POT-padding UV correction for the bound texture (real/padded). Same for all 3 verts.
    float pvr_us = usetex ? gfx_pvr_get_u_scale() : 1.0f;
    float pvr_vs = usetex ? gfx_pvr_get_v_scale() : 1.0f;
    // Ortho 3D GEOMETRY (depth-tested, e.g. goddard's head): under ortho w is ~constant, so 1/w
    // carries NO depth — derive depth from the clip-space z instead (below). This is NOT a 2D
    // overlay; without this it z-fights/renders inside-out (faces resolve in submit order).
    int ortho_3d = proj_is_ortho && depth_test;
    // 2D-as-triangles (ortho, Z-OFF menu/HUD/text) is a foreground OVERLAY when there's no 3D
    // scene this frame, OR 3D has ALREADY drawn (painted on top, e.g. file-select text over the
    // save faces) -> near screen_2d_z paint order. A Z-off ortho draw BEFORE any 3D in a 3D-scene
    // frame is a true BACKDROP and keeps the far-pin. (Mirrors gfx_draw_rectangle.)
    int ortho_overlay = proj_is_ortho && !depth_test && (!prev_frame_had_persp || has_done_3d);
    float z2d = 0.0f;
    if (ortho_overlay) { screen_2d_z += 0.005f; z2d = screen_2d_z; }

    // Peach<->Bowser painting trilerp workaround (the one G_CC_TRILERP dual-tile effect in the
    // game). The fake DL draws BOTH portraits over the SAME coincident geometry, cross-faded by
    // trilerp_a; bowser is drawn first (computes trilerp_a from Mario's distance), peach second.
    // Approximate the hardware 2-tile lerp as two single-texture alpha-blended passes: override
    // each vertex's alpha here. The combiner is DECALRGB -> PVR DECAL, whose final alpha = col.a
    // (the vertex/shade alpha), so the override reaches the framebuffer blend. Skip a pass that's
    // fully faded out, and nudge peach toward the camera (larger 1/w) so it wins the z-fight.
    uint8_t trilerp_alpha = 0;
    int trilerp_pass = 0;          // 0 = not trilerp, 1 = peach (z-biased), 2 = bowser
    if (in_trilerp) {
        if (doing_peach) {
            if (!trilerp_a) return;            // peach fully faded -> bowser shown alone
            trilerp_alpha = trilerp_a; trilerp_pass = 1;
        } else if (doing_bowser) {
            if (trilerp_a == 255) return;      // bowser fully faded -> peach shown alone
            trilerp_alpha = 255 - trilerp_a; trilerp_pass = 2;
        }
    }

    // No buf_vbo on PVR. OP (kind 0) bakes this near-clip fan into the op_emit scratch then DR-submits
    // it after the fan; PT/TR (kind 1/2) bake DIRECTLY into their deferred bucket (reserved here).
    // `emit` is the fan's destination; op_n is the per-fan vertex counter.
    const int op_stream = (pvr_cur_kind == 0);
    size_t op_n = 0;
    dc_fast_t *emit = op_stream ? op_emit : pvr_reserve(pvr_cur_kind, (size_t) n_tris * 3);
    if (!emit) n_tris = 0;   // bucket overflow -> drop this source triangle (pvr_reserve logged it)
    for (int ti = 0; ti < n_tris; ti++) {
        v_arr[0] = fan_tris[ti][0];
        v_arr[1] = fan_tris[ti][1];
        v_arr[2] = fan_tris[ti][2];

    for (i = 0; i < 3; i++) {
        dc_fast_t * const bv = &emit[op_n];
        // Raw-PVR: perspective divide + viewport map -> screen pixels; z = 1/w (inverse depth).
        // Z-disabled geometry (skybox) far-pinned so the depth-tested scene wins.
        float invw = shz_fast_invf(v_arr[i]->_w);
        bv->vert.x = sm_xscale * (v_arr[i]->_x * invw) + sm_xbias;
        bv->vert.y = sm_yscale * (v_arr[i]->_y * invw) + sm_ybias;
        // Decal coplanar surfaces (N64 ZMODE_DEC) z-fight the surface they sit on. PVR z is 1/w
        // (GEQUAL, larger == nearer), so nudge the decal's z slightly LARGER -> it wins. This is
        // the PVR analogue of GLdc's "push the geometry toward the camera" decal hack. The bias
        // is MULTIPLICATIVE: PVR stores z as a float, so a proportional nudge tracks the per-depth
        // ULP (a flat additive epsilon would over/under-bias near vs far). PVR_DECAL_ZBIAS tunable.
        // ortho 3D: depth from clip z (ndc_z = z/w in [-1 near .. +1 far]); 1-ndc_z gives PVR
        // depth with larger==nearer, matching the GEQUAL/1-over-w convention. Perspective: 1/w.
        float dz;
        if (ortho_3d)        dz = 1.0f - (v_arr[i]->_z * invw);
        else if (depth_test) dz = (zmode_decal || trilerp_pass == 1) ? invw * PVR_DECAL_ZBIAS : invw;
        else                 dz = 0.00001f;
        bv->vert.z = ortho_overlay ? z2d : dz;

        if (usetex) {
            float u = (v_arr[i]->u - (rdp.texture_tile.uls << 3)) * 0.03125f;// / 32.0f;
            float v = (v_arr[i]->v - (rdp.texture_tile.ult << 3)) * 0.03125f;// / 32.0f;
            if (linear_filter) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            bv->texture.u = u * recip_tw * pvr_us;
            bv->texture.v = v * recip_th * pvr_vs;
        }

        // Raw-PVR: evaluate the N64 colour+alpha combiner directly (replaces packedc). argb =
        // modulate colour, oargb = additive offset (post-modulate). usetex == poly is textured.
        {
            uint32_t _argb, _oargb;
            // Trilerp passes override the vertex alpha (cross-fade factor); DECAL routes col.a to
            // the final alpha. Use a local copy so the shared (re-used) vertex isn't mutated.
            struct RGBA sh_local = v_arr[i]->color;
            if (trilerp_pass) sh_local.a = trilerp_alpha;
            pvr_eval_combiner(rdp.combine_w0, rdp.combine_w1, &sh_local, usetex, texenv, &_argb, &_oargb);
            // PVR hardware vertex fog reads the density from the offset-colour (oargb) ALPHA. The
            // combiner only ever sets oargb RGB (offset is alpha-0), so OR in this vertex's fog
            // coefficient — the two share oargb without conflict.
            _oargb |= (uint32_t) v_arr[i]->fog << 24;
            bv->color.packed = _argb;
            bv->pad0.vertindex = _oargb;
        }

        op_n += 1;
	}
    }   // close the near-clip fan loop (for ti)
    // OP baked its whole fan into the scratch — stream it to the live OP list now (PT/TR are already
    // in their bucket). pvr_emit_op_header re-emits only on state change, so same-state runs stream headerless.
    if (op_stream && op_n) pvr_submit_op(op_emit, op_n);
    // A perspective 3D primitive has drawn this frame -> a later Z-off ortho 2D draw is a
    // foreground overlay, not a backdrop (gfx_draw_rectangle's far-pin reads this).
    if (!proj_is_ortho) has_done_3d = 1;
}

extern void gfx_opengl_draw_triangles_2d(void *buf_vbo, size_t buf_vbo_len, size_t buf_vbo_num_tris);

int do_ext_fill = 0;

static void __attribute__((noinline)) gfx_sp_quad_2d(void) {
    dc_fast_t* v2d = &rsp.loaded_vertices_2D[0];

#ifdef GFX_BACKEND_PVR
    // A guaranteed-OPAQUE 2D quad lands on the live OP list (renders before TR). For it to
    // cover TR geometry it must WRITE DEPTH at its near 2D z, or TR paints over it (the intro
    // fade "disappearing" once it goes fully opaque). Force z-write for opaque 2D; translucent
    // 2D stays on TR and needs no force. opaque == no blend alpha AND no texel-driven alpha.
    uint8_t opaque_2d;
    {
        uint8_t ua = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
        if ((rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA) ua = 1;
        int aa = (rdp.combine_w0 >> 12) & 7, ab = (rdp.combine_w1 >> 12) & 7,
            ac = (rdp.combine_w0 >>  9) & 7, ad = (rdp.combine_w1 >>  9) & 7;
        uint8_t aut = (aa==1||aa==2||ab==1||ab==2||ac==1||ac==2||ad==1||ad==2);
        opaque_2d = !(ua || aut);
    }
#endif

    // for reasons, this is always 0? why?
    uint8_t depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    uint8_t z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
#ifdef GFX_BACKEND_PVR
    if (opaque_2d) z_upd = 1;   // write depth at the near 2D z so TR can't paint over it
#endif
    if (z_upd != rendering_state.depth_mask) {
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }

    uint8_t zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            n64_memcpy(&rendering_state.viewport, &rdp.viewport, sizeof(rdp.viewport));
#ifdef GFX_BACKEND_PVR
            // PVR backend's set_viewport is a no-op; the front-end owns the screen map.
            vpf_x = rdp.viewport.x; vpf_y = rdp.viewport.y;
            vpf_w = rdp.viewport.width; vpf_h = rdp.viewport.height;
            gfx_recompute_screen_map();
#endif
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            n64_memcpy(&rendering_state.scissor, &rdp.scissor, sizeof(rdp.scissor));
        }
        rdp.viewport_or_scissor_changed = 0;
    }

    uint32_t cc_id = rdp.combine_mode;

    uint8_t use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    uint8_t use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    if (rsp.use_fog != use_fog) {
        rsp.use_fog = use_fog;
    }

    uint8_t texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;

    // this is literally only for the stupid sun in the intro and nothing else
    uint8_t use_noise = (rdp.other_mode_h == 0x2ca0);

    if (texture_edge) {
        use_alpha = 1;
    }

    if (use_alpha) {
        cc_id |= SHADER_OPT_ALPHA;
    }

    if (use_fog) {
        cc_id |= SHADER_OPT_FOG;
    }

    if (texture_edge) {
        cc_id |= SHADER_OPT_TEXTURE_EDGE;
    }

    if (use_noise) {
        cc_id |= SHADER_OPT_NOISE;
    }

    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }

    struct ColorCombiner* comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram* prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }

    if (use_alpha != rendering_state.alpha_blend) {
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }

    uint8_t num_inputs;
    uint8_t use_texture = !do_ext_fill &&gfx_rapi->shader_get_info(prg, &num_inputs);
    uint8_t linear_filter = 1;

    dc_fast_t* tmpv = v2d;

    if (use_texture) {
        if (rdp.texture_changed) {
            import_texture();
            rdp.texture_changed = 0;
        }
        linear_filter = ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT);
        gfx_rapi->set_sampler_parameters(linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
        rendering_state.texture->linear_filter = linear_filter;
        rendering_state.texture->cms = rdp.texture_tile.cms;
        rendering_state.texture->cmt = rdp.texture_tile.cmt;
        uint32_t tex_width = ((rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) * 0.25f);
        uint32_t tex_height = ((rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) * 0.25f);
        float recip_tex_width = shz_fast_invf((float) tex_width);
        float recip_tex_height = shz_fast_invf((float) tex_height);
        float offs = linear_filter ? 0.5f : 0.0f;
        float uls = (float) (rdp.texture_tile.ult * 0.25f) - offs;
        float ult = (float) (rdp.texture_tile.ult * 0.25f) - offs;
        float u;
        float v;

        // / 32
        u = (tmpv->texture.u * 0.03125f) - uls;
        tmpv->texture.u = (u * recip_tex_width);
        v = (tmpv->texture.v * 0.03125f) - ult;
        tmpv++->texture.v = (v * recip_tex_height);

        u = (tmpv->texture.u * 0.03125f) - uls;
        tmpv->texture.u = (u * recip_tex_width);
        v = (tmpv->texture.v * 0.03125f) - ult;
        tmpv++->texture.v = (v * recip_tex_height);

        u = (tmpv->texture.u * 0.03125f) - uls;
        tmpv->texture.u = u * recip_tex_width;
        v = (tmpv->texture.v * 0.03125f) - ult;
        tmpv++->texture.v = v * recip_tex_height;

        u = (tmpv->texture.u * 0.03125f) - uls;
        tmpv->texture.u = u * recip_tex_width;
        v = (tmpv->texture.v * 0.03125f) - ult;
        tmpv->texture.v = v * recip_tex_height;
    }

    uint32_t rectcolor = 0xffffffff;

    int k;
    for (k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
        switch (comb->shader_input_mapping[k][0]) {
            case CC_PRIM:
                rectcolor = PACK_ARGB8888(rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, k ? rdp.prim_color.a : 255);
                break;
            case CC_SHADE:
                v2d[0].color.array.a = k ? v2d[0].color.array.a : 255;
                rectcolor = v2d[0].color.packed;
                break;
            case CC_ENV:
                rectcolor = PACK_ARGB8888(rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, k ? rdp.env_color.a : 255);
                break;
            default:
                rectcolor = 0xffffffff;
                break;
        }
    }

    v2d++->color.packed = rectcolor;
    v2d++->color.packed = rectcolor;
    v2d++->color.packed = rectcolor;
    v2d->color.packed = rectcolor;

#ifdef GFX_BACKEND_PVR
    {
        int aa = (rdp.combine_w0 >> 12) & 7, ab = (rdp.combine_w1 >> 12) & 7;
        int ac = (rdp.combine_w0 >>  9) & 7, ad = (rdp.combine_w1 >>  9) & 7;
        uint8_t alpha_uses_texel = (aa == 1 || aa == 2 || ab == 1 || ab == 2 ||
                                    ac == 1 || ac == 2 || ad == 1 || ad == 2);
        uint8_t alpha_compare_on = (rdp.other_mode_l & 3) != 0;
        uint8_t real_blend = (rdp.other_mode_l & FORCE_BL) != 0;
        uint8_t cutout     = texture_edge || (alpha_uses_texel && alpha_compare_on);
        uint8_t kind = real_blend ? 2 : (cutout ? 1 : 0);
        if (kind != pvr_cur_kind) { gfx_pvr_set_blend(kind); pvr_cur_kind = kind; }
        if (kind == 2) {
            uint8_t bs, bd;
            pvr_derive_blend(rdp.other_mode_l, rdp.other_mode_h, &bs, &bd);
            if (bs != pvr_cur_bsrc || bd != pvr_cur_bdst) {
                gfx_pvr_set_blend_factors(bs, bd);
                pvr_cur_bsrc = bs; pvr_cur_bdst = bd;
            }
        }
        // Derive + set the texenv from THIS quad's combiner. Unlike gfx_sp_tri1, the 2D texrect path
        // had no texenv step, so it inherited a STALE texenv from the last geometry draw — e.g. the
        // wood COURSE panel leaving REPLACE made the black act-name / star-number menu glyphs render
        // as raw white texel (env colour dropped) -> invisible on the white star-select background.
        uint32_t texenv = use_texture ? derive_pvr_texenv(rdp.combine_mode) : GFX_TEXENV_MODULATE;
        if (texenv != rendering_state.tex_env) {
            gfx_rapi->set_tex_env(texenv);
            rendering_state.tex_env = texenv;
        }
    }
#endif
    gfx_rapi->draw_triangles_2d((void*) rsp.loaded_vertices_2D, 4, use_texture);
}

static void  gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = /* 2.0f * */ viewport->vscale[0] * 0.5f /* / 4.0f */;
    float height = /* 2.0f * */ viewport->vscale[1] * 0.5f /* / 4.0f */;
    float x = (viewport->vtrans[0] * 0.25f /* / 4.0f */) - width * 0.5f /* / 2.0f */;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] * 0.25f /* / 4.0f */) + height * 0.5f /* / 2.0f */);
    
    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}
static void  __attribute__((noinline)) gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            } else {
                if (lightidx == -2)
                    memcpy(rsp.current_lookat, data, sizeof(Light_t));
                else if (lightidx == -1)
                    memcpy(rsp.current_lookat + 1, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}


extern float gProjectNear;
extern float gProjectFar;

// the following isn't very rigorous
// I eyeballed it and tweaked the `a` param until it seemed almost right
// and still need to scale it more per-level in `gfx_gldc` when setting gl fog params
static inline float exp_map_0_1000_f(float x) {
    const float a = 138.62943611198894f;
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1000.0f)
        return x * 1.01f; // 1000.0f;

    const float t = (x * 0.001f);
    const float num = expm1f(a * (t - 1.0f));
    const float den = expm1f(-a);
    return 1000.0f * (1.0f - shz_divf(num, den));
}
//#undef UNUSED
//#define UNUSED

static void gfx_sp_moveword(uint8_t index, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) >> 5;// / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc) {
    rsp.s_scale = sc;
    rsp.t_scale = tc;
}

static void  __attribute__((noinline)) gfx_dp_set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = (ulx * 0.25f) * RATIO_X;
    float y = (SCREEN_HEIGHT - lry * 0.25f) * RATIO_Y;
    float width = ((lrx - ulx) * 0.25f) * RATIO_X;
    float height = ((lry - uly) * 0.25f) * RATIO_Y;
    
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t size, uint32_t width, const void* addr) {
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
	last_set_texture_image_width = width;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint8_t tile, uint32_t cmt, uint32_t cms) {
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
        rdp.texture_tile.line_size_bytes = line * 8;
        rdp.texture_changed = true;
    }
}

static void  gfx_dp_set_tile_size(uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;
    rdp.texture_changed = true;
}

static void  gfx_dp_load_block(uint32_t lrs) {
    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift = 0;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t size_bytes = (lrs + 1) << word_size_shift;

    rdp.loaded_texture.size_bytes = size_bytes;
    rdp.loaded_texture.addr = rdp.texture_to_load.addr;

    rdp.texture_changed = true;
}

static void  gfx_dp_load_tile(uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    uint32_t word_size_shift = 0;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t size_bytes = (((lrs >> G_TEXTURE_IMAGE_FRAC) + 1) * ((lrt >> G_TEXTURE_IMAGE_FRAC) + 1)) << word_size_shift;

    rdp.loaded_texture.size_bytes = size_bytes;
    rdp.loaded_texture.addr = rdp.texture_to_load.addr;

    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;

    rdp.texture_changed = true;
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha) {
    rdp.combine_mode = rgb | (alpha << 12);
}

int env_a;

static void   gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
    env_a = a;
}

static void  gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

#define recip255 0.00392157f

static void   gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
    if ((!rendering_state.fog_col_change)) {
        rendering_state.fog_col_change = 1;
        // First fog colour of the frame wins (fog_col_change is reset per frame in gfx_sp_reset).
        // The PVR fog-colour register is a single global, so this same guard keeps it set once.
#ifdef GFX_BACKEND_PVR
        gfx_pvr_set_fog_color(r, g, b, a);
#else
        float fog_color[4] = { rdp.fog_color.r * recip255, rdp.fog_color.g * recip255, rdp.fog_color.b * recip255,
                               1.0f };
        glFogfv(GL_FOG_COLOR, fog_color);
#endif
    }
}

static void  gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = r << 3;
    rdp.fill_color.g = g << 3;
    rdp.fill_color.b = b << 3;
    rdp.fill_color.a = a ? 255 : 0;
}

void gfx_opengl_2d_projection(void) {
#ifndef GFX_BACKEND_PVR
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0f, 640.0f, 480.0f, 0.0f, -10000.0f, 10000.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#endif
    matrix_dirty = 1;
}

void gfx_opengl_reset_projection(void) {
    matrix_dirty = 1;
}

static void  __attribute__((noinline)) gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

#if 0
    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = (ulyf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = (lryf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
#endif    
    ulxf = ulxf * 0.0015625f - 1.0f;
    ulyf = ulyf * 0.00208333f - 1.0f;
    lrxf = lrxf * 0.0015625f - 1.0f;
    lryf = lryf * 0.00208333f - 1.0f;

    ulxf = ulxf;
    lrxf = lrxf;

    ulxf = (ulxf * 320.0f) + 320.0f;
    lrxf = (lrxf * 320.0f) + 320.0f;

    ulyf = (ulyf * 240.0f) + 240.0f;
    lryf = (lryf * 240.0f) + 240.0f;
    
#ifdef GFX_BACKEND_PVR
    // Build the 4 verts in PVR triangle-STRIP order (ul, ll, ur, lr) so the backend submits
    // them straight as a native quad. (GLdc's GL_QUADS uses loop order ul, ll, lr, ur below.)
    dc_fast_t *ul = &rsp.loaded_vertices_2D[0];
    dc_fast_t *ll = &rsp.loaded_vertices_2D[1];
    dc_fast_t *ur = &rsp.loaded_vertices_2D[2];
    dc_fast_t *lr = &rsp.loaded_vertices_2D[3];
    // 2D paint-order depth: PVR z = 1/w, larger == nearer; the per-rect increment encodes
    // submission order so the PT/TR lists resolve 2D-among-2D layering.
    screen_2d_z += 0.005f;
    float rz = screen_2d_z;
    // A 2D quad — textured OR a solid fillrect — drawn BEFORE any 3D, in a frame that HAS a 3D
    // scene, is a BACKDROP: the goddard/title background, OR geo_process_background's portal/
    // opening fill (white star-select entry, black behind opening doorways). Far-pin it so the
    // 3D scene draws over it and it shows through only the openings. Fades/effect fills come
    // AFTER 3D (has_done_3d == 1), so draw-order alone discriminates them — no need for the old
    // `!do_ext_fill` exclusion (which wrongly kept backdrop fills near; do_ext_fill is also a
    // GLdc-era flag we don't want the depth logic leaning on).
    if (prev_frame_had_persp && !has_done_3d) rz = 0.00001f;
#else
    dc_fast_t *ul = &rsp.loaded_vertices_2D[0];
    dc_fast_t *ll = &rsp.loaded_vertices_2D[1];
    dc_fast_t *lr = &rsp.loaded_vertices_2D[2];
    dc_fast_t *ur = &rsp.loaded_vertices_2D[3];
    float rz = 1.0f;
#endif

    ul->vert.x = ulxf;
    ul->vert.y = ulyf;
    ul->vert.z = rz;

    ll->vert.x = ulxf;
    ll->vert.y = lryf;
    ll->vert.z = rz;

    lr->vert.x = lrxf;
    lr->vert.y = lryf;
    lr->vert.z = rz;

    ur->vert.x = lrxf;
    ur->vert.y = ulyf;
    ur->vert.z = rz;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    gfx_sp_quad_2d();

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}
#define seg_addr(a) ((void*)a)

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

static void  __attribute__((noinline)) gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint32_t saved_combine_mode = rdp.combine_mode;

    do_ext_fill = 0;

    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0));
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
#ifdef GFX_BACKEND_PVR
    // strip order (must match gfx_draw_rectangle): ul, ll, ur, lr
    dc_fast_t *ul = &rsp.loaded_vertices_2D[0];
    dc_fast_t *ll = &rsp.loaded_vertices_2D[1];
    dc_fast_t *ur = &rsp.loaded_vertices_2D[2];
    dc_fast_t *lr = &rsp.loaded_vertices_2D[3];
#else
    dc_fast_t *ul = &rsp.loaded_vertices_2D[0];
    dc_fast_t *ll = &rsp.loaded_vertices_2D[1];
    dc_fast_t *lr = &rsp.loaded_vertices_2D[2];
    dc_fast_t *ur = &rsp.loaded_vertices_2D[3];
#endif
    ul->texture.u = uls;
    ul->texture.v = ult;
    lr->texture.u = lrs;
    lr->texture.v = lrt;
    if (!flip) {
        ll->texture.u = uls;
        ll->texture.v = lrt;
        ur->texture.u = lrs;
        ur->texture.v = ult;
    } else {
        ll->texture.u = lrs;
        ll->texture.v = ult;
        ur->texture.u = uls;
        ur->texture.v = lrt;
    }
    
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    int i;

    do_ext_fill = 1;

#ifndef GFX_BACKEND_PVR
    // GLdc keeps the stock Fast3D guard: skip the z-buffer-clear fillrect (the one case where the
    // game points the color image at the z buffer, so cimg==zimg). On the PVR path it must NOT
    // run: this port's clear_z_buffer() is a no-op and G_SETCIMG/G_SETZIMG are never emitted, so
    // BOTH addresses stay 0 -> 0==0 would skip EVERY fillrect (incl. geo_process_background's
    // backdrop, screen borders, fades). No real z-clear fillrects exist here, so PVR draws all.
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
#endif
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    uint32_t saved_geom_mode = rsp.geometry_mode;
    uint32_t saved_other_mode_l = rdp.other_mode_l;
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    for (i = 0; i < 4; i++) {
        dc_fast_t *v = &rsp.loaded_vertices_2D[i];
        v->color.array.a = rdp.fill_color.a;
        v->color.array.b = rdp.fill_color.b;
        v->color.array.g = rdp.fill_color.g;
        v->color.array.r = rdp.fill_color.r;
    }
    
    gfx_draw_rectangle(ulx, uly, lrx, lry);

    rsp.geometry_mode = saved_geom_mode;
    rdp.other_mode_l = saved_other_mode_l;
    do_ext_fill = 0;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

extern Gfx *ddd_dl;

extern Gfx inside_castle_seg7_dl_07037DE8[];
extern Gfx g_hmc_seg7_dl_07014950[];
extern Gfx g_hmc_seg7_dl_0700FDF0[];
extern Gfx dl_draw_text_bg_box[];
extern Gfx mr_i_eyeball_seg6_dl_06002080[];
extern Gfx water_bubble_seg5_dl_05010D30[];
extern Gfx dl_ia_text_begin[];
extern Gfx dl_ia_text_end[];
extern Gfx dl_menu_idle_hand[];
extern Gfx dl_menu_grabbing_hand[];
extern Gfx dl_menu_hand[];
extern Gfx cotmc_seg7_dl_0700A4B8[];
extern Gfx water_ring_seg6_dl_06013AC0[];
extern Gfx cotmc_dl_water_begin[];
extern Gfx cotmc_dl_water_end[];
extern Gfx dl_paintings_env_mapped_begin[];
extern Gfx dl_paintings_env_mapped_end[];
extern Gfx dl_transition_draw_filled_region[];
extern Gfx dl_screen_transition_end[];
extern Gfx dl_draw_quad_verts_0123[];


#define GFX_DL_STACK_MAX 8 /* tune this to whatever nesting you expect */

static Gfx __attribute__((aligned(32))) * dl_stack[GFX_DL_STACK_MAX];

//int max_sp = 0;

static void __attribute__((noinline)) gfx_run_dl(Gfx* cmd) {
    int dl_sp = 0;

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;

        __builtin_prefetch(clip_rej);

        if (cmd->words.w0 == 0x424C4E44) {
            __builtin_prefetch((void *) (cmd) + 32);
            if (cmd->words.w1 == 0xAAAAAAAA) {
                in_trilerp = 1;
                doing_peach = 0;
                doing_bowser = 0;
            } else if (cmd->words.w1 == 0xBBBBBBBB) {
                doing_peach = 1;
                doing_bowser = 0;
            } else if (cmd->words.w1 == 0xCCCCCCCC) {
                doing_bowser = 1;
                doing_peach = 0;
            } else if (cmd->words.w1 == 0xDDDDDDDD) {
                in_trilerp = 0;
                doing_bowser = 0;
                doing_peach = 0;
            }
            ++cmd;
            continue;
        }

        switch (opcode) {
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;

            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;

            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;

            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;

            case G_DL:
                __builtin_prefetch(seg_addr(cmd->words.w1));
                if (C0(16, 1) == 0) {
                  
                    dl_stack[dl_sp++] = cmd + 1; /* return to next command */
//                    if (dl_sp > max_sp) {
//                        printf("max DL stack depth %d\n", dl_sp);
//                        max_sp = dl_sp;
//                    }
                    cmd = (Gfx*) seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                    // Push return address
//                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                if (dl_sp == 0) {
                    /* top-level ENDDL: we're done */
                    return;
                } else {
                    /* pop return address and resume caller DL */
                    cmd = dl_stack[--dl_sp];
                    __builtin_prefetch(cmd);
                    --cmd; /* ++cmd at loop bottom -> first command after the call */
                }
                break;
            //                return;

#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif

            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(17, 7), C0(9, 7), C0(1, 7));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(17, 7), C1(9, 7), C1(1, 7));
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI) || defined(F3DEX_GBI_2)
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(17, 7), C0(9, 7), C0(1, 7));
                gfx_sp_tri1(C1(17, 7), C1(9, 7), C1(1, 7));
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(19, 2), C0(0, 10), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(12, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C1(24, 3), C1(18, 2), C1(8, 2));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(
                    color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)));
                // raw words for the PVR combiner evaluator (the compressed cc_id drops
                // G_CCMUX_1 + the true alpha mux), same bit layout the eval expects.
                rdp.combine_w0 = cmd->words.w0;
                rdp.combine_w1 = cmd->words.w1;
                break;
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
//                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(seg_addr(cmd->words.w1));
                break;
        }

        __builtin_prefetch((void*) (++cmd) + 32);
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rendering_state.fog_col_change = 0;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    size_t i;

    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();
#if 1
    // Used in the 120 star TAS
    static uint32_t precomp_shaders[] = {
        0x01200200,
        0x00000045,
        0x00000200,
        0x01200a00,
        0x00000a00,
        0x01a00045,
        0x00000551,
        0x01045045,
        0x05a00a00,
        0x01200045,
        0x05045045,
        0x01045a00,
        0x01a00a00,
        0x0000038d,
        0x01081081,
        0x0120038d,
        0x03200045,
        0x03200a00,
        0x01a00a6f,
        0x01141045,
        0x07a00a00,
        0x05200200,
        0x03200200,
        0x09200200,
        0x0920038d,
        0x09200045
    };
    for (i = 0; i < sizeof(precomp_shaders) / sizeof(uint32_t); i++) {
        gfx_lookup_or_create_shader_program(precomp_shaders[i]);
    }
#endif
    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }

    rsp.current_lookat[0].dir[0] = 0;
    rsp.current_lookat[0].dir[1] = 127;
    rsp.current_lookat[0].dir[2] = 0;
    rsp.current_lookat[1].dir[0] = 127;
    rsp.current_lookat[1].dir[1] = 0;
    rsp.current_lookat[1].dir[2] = 0;


    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

void gfx_start_frame(void) {
    gfx_wapi->handle_events();
}

void gfx_run(Gfx *commands) {
    __builtin_prefetch(commands);
    gfx_sp_reset();
    
    gfx_rapi->start_frame();
    gfx_run_dl(commands);
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
}

void gfx_end_frame(void) {
    gfx_rapi->finish_render();
    gfx_wapi->swap_buffers_end();
}
