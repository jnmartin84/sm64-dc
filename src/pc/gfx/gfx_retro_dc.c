#if 1
//defined(TARGET_DC)
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
//#include <stdbool.h>
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

#if defined(TARGET_DC)
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>
#include <GL/glkos.h>
#include "gl_fast_vert.h"
#endif

int in_trilerp = 0;
int doing_peach = 0;
int doing_bowser = 0;
int drawing_hand = 0;
int do_radar_mark = 0;

#define SUPPORT_CHECK(x) assert(x)
int aquarium_draw = 0;
int water_bomb = 0;
int doing_skybox = 0;
int font_draw = 0;
// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED (256)
#define MAX_LIGHTS 2
#define MAX_VERTICES 32

#undef bool
#define bool uint8_t
#define true 1
#define false 0

int doing_hmc_thing = 0;
int doing_text_bg_box = 0;

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

struct RGBA {
    uint8_t r, g, b, a;
};

struct XYWidthHeight {
    uint16_t x, y, width, height;
};

#if 0
struct LoadedVertex {
    float x, y, z, w;
    float _x, _y, _z, _w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
};
#endif

// exactly 32 bytes
struct __attribute__((aligned(32))) LoadedVertex {
    // 0, 4, 8, 12
    float _x, _y /* , _z, _w */;
    // 16, 20, 24, 28
    float x, y, z /* , w */;
    // 32, 36
    float u, v;
    // 40
    struct RGBA color;
};

struct __attribute__((aligned(16))) LoadedNormal {
    float x, y, z, w;
};

// bits 0 - 5 -> clip_rej
// bit 6 - wlt0
// bit 7 - lit
uint8_t __attribute__((aligned(32))) clip_rej[MAX_VERTICES];


struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
};
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
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
    struct LoadedVertex loaded_vertices[MAX_VERTICES+4];
    struct LoadedNormal loaded_normals[MAX_VERTICES+4];
    struct __attribute__((aligned(32))) dc_fast_t loaded_vertices_2D[4];

    float MP_matrix[4][4] __attribute__((aligned(32)));
    float P_matrix[4][4] __attribute__((aligned(32)));
    float modelview_matrix_stack[11][4][4] __attribute__((aligned(32)));

    uint8_t modelview_matrix_stack_size;
    Light_t __attribute__((aligned(32))) current_lights[MAX_LIGHTS + 1];
    float __attribute__((aligned(32))) current_lights_coeffs[MAX_LIGHTS][3];
    float __attribute__((aligned(32))) current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    uint8_t use_fog;
} rsp __attribute__((aligned(32)));

static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
    } loaded_texture[2];
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp __attribute__((aligned(32)));

static struct RenderingState {
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    // 28
    uint8_t fog_change;
    // 29
    uint8_t fog_col_change;

} rendering_state __attribute__((aligned(32)));

struct GfxDimensions gfx_current_dimensions;

static bool dropped_frame;
static int first_2d_in_frame;

static dc_fast_t __attribute__((aligned(32))) buf_vbo[MAX_BUFFERED * 3]; // 3 vertices in a triangle
//static dc_fast_t __attribute__((aligned(32))) quad_vbo[2 * 3]; // 2 tris make a quad
static size_t buf_vbo_len = 0;
static size_t buf_num_vert = 0;
static size_t buf_vbo_num_tris = 0;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

/*@Note: Unused */
#if 0
#include <time.h>
static unsigned long get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        //int num = buf_vbo_num_tris;
        //unsigned long t0 = get_time();
        gfx_rapi->draw_triangles((void *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_num_vert = 0;
        buf_vbo_num_tris = 0;
        //unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}
uint16_t tlut[256];
int 	last_set_texture_image_width=0;
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
    gfx_flush();
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

void gfx_clear_texidx(unsigned int texidx);

void reset_texcache(void) {
    gfx_texture_cache.pool_pos = 0;
    memset(&gfx_texture_cache, 0, sizeof(gfx_texture_cache));
}


static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz) {
    size_t hash = (uintptr_t)orig_addr;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    //if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
//        exit(-1);
        // Pool is full. We just invalidate everything and start over.
//        gfx_texture_cache.pool_pos = 0;
  //  node = &gfx_texture_cache.hashmap[hash];
       //puts("Clearing texture cache");
    //}
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    *n = *node;
    return false;
}

#if 1
uint16_t __attribute__((aligned(32))) rgba16_buf[4096 * 2];

static void import_texture(int tile);

static void import_texture_rgba16(int tile) {
	uint32_t i;
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    uint16_t *inbuf = (uint16_t*)rdp.loaded_texture[tile].addr;

		for (i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
			uint16_t col16 = inbuf[i];
            col16 = (col16 << 8) | ((col16 >> 8) & 0xff);
            rgba16_buf[i] = ((col16 & 1) << 15) | (col16 >> 1);
		}

		gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void import_texture_rgba32(int tile) {
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = (rdp.loaded_texture[tile].size_bytes / 2) / rdp.texture_tile.line_size_bytes;

    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t r = (rdp.loaded_texture[tile].addr[(4 * i) + 0] >> 4) & 0x0f;
        uint8_t g = (rdp.loaded_texture[tile].addr[(4 * i) + 1] >> 4) & 0x0f;
        uint8_t b = (rdp.loaded_texture[tile].addr[(4 * i) + 2] >> 4) & 0x0f;
        uint8_t a = (rdp.loaded_texture[tile].addr[(4 * i) + 3] >> 4) & 0x0f;
        rgba16_buf[i] = (a << 12) | (r << 8) | (g << 4) | (b);
    }

    gfx_rapi->upload_texture((uint8_t *) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

uint8_t __attribute__((aligned(32))) xform_buf[8192];

static void import_texture_ia4(int tile) {
    uint32_t width = rdp.texture_tile.line_size_bytes << 1;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    uint32_t i;

    for (i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;

//SCALE_3_8(VAL_) ((VAL_) << 5)
//((part >> 1) << 5) >> 3
        uint8_t intensity = (part & 0xE) << 1;//(SCALE_3_8(part >> 1) >> 3) & 0x1f;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        uint16_t col16 = (alpha << 15) | (r << 10) | (g << 5) | (b);
        rgba16_buf[i] = col16;
    }

    gfx_rapi->upload_texture((uint8_t *) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void import_texture_ia8(int tile) {
    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t val = rdp.loaded_texture[tile].addr[i];
        uint8_t in = ((val >> 4) & 0xf);
        uint8_t al = (val & 0xf);
        rgba16_buf[i] = (al << 12) | (in << 8) | (in << 4) | in;
    }

    gfx_rapi->upload_texture((uint8_t *) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void import_texture_ia16(int tile) {
	uint32_t i;
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	u32 src_width;
	if (last_set_texture_image_width) {
		src_width = last_set_texture_image_width + 1;
	} else {
		src_width = width;
	}

	uint16_t* start = (uint16_t *)rdp.loaded_texture[tile].addr;
	if (last_set_texture_image_width) {
		start =
			(uint16_t*) &rdp.loaded_texture[tile]
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

static void import_texture_i4(int tile) {
	int width = rdp.texture_tile.line_size_bytes << 1;
	int height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

	height = (height + 3) & ~3;

	if (last_set_texture_image_width == 0) {
		for (int i = 0; i < (int)rdp.loaded_texture[tile].size_bytes; i++) {
			uint16_t idx = (i<<1);
			uint8_t byte = rdp.loaded_texture[tile].addr[i];
			uint8_t part1,part2;
			part1 = (byte >> 4) & 0xf;
			part2 = byte & 0xf;
			rgba16_buf[idx  ] = (part1 << 12) | (part1 << 8) | (part1 << 4) | part1;
			rgba16_buf[idx+1] = (part2 << 12) | (part2 << 8) | (part2 << 4) | part2;
		}
	} else {
		memset(rgba16_buf, 0, 8192*sizeof(uint16_t));
		uint8_t* start =
			(uint8_t*) &rdp.loaded_texture[tile]
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

static void import_texture_i8(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

	memset(xform_buf, 0, width*height*2);

	uint8_t* start = (uint8_t*) &rdp.loaded_texture[tile]
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

	for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
		uint8_t in = (xform_buf[i] >> 4) & 0xf;
		rgba16_buf[i] = (in << 12) | (in << 8) | (in << 4) | in;
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void import_texture_ci4(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes * 2;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	uint32_t i;
//memset(rgba16_buf, 0, width*height);

//	if (last_set_texture_image_width == 0) {
		for (i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
			uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
			uint8_t part;
			if (!(i & 1)) {
				part = (byte >> 4) & 0xf;
			} else {
				part = byte & 0xf;
			}
			rgba16_buf[i] = tlut[part];
		}
#if 0
	} else {
		uint8_t xform_buf[8192];
	//	memset(xform_buf, 0, width*height*2);
		uint8_t* start =
			(uint8_t*) &rdp.loaded_texture[tile]
				.addr[(((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) / 2) * (last_set_texture_image_width + 1)) +
					  ((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC) / 2)];
		for (uint32_t i = 0; i < height; i++) {
			for (uint32_t x = 0; x < (last_set_texture_image_width + 1) * 2; x += 2) {
				if (i & 1) {
					xform_buf[(i * (width / 2)) + (x)] = (start[(x / 2)] & 0xf);
					xform_buf[((i * (width / 2)) + (x)) + 1] = (start[(x / 2)] >> 4) & 0xf;

				} else {
					xform_buf[(i * (width / 2)) + (x)] = (start[(x / 2)] >> 4) & 0xf;
					xform_buf[((i * (width / 2)) + (x)) + 1] = (start[(x / 2)] & 0xf);
				}
			}
			start += (last_set_texture_image_width + 1);
		}
		for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i+=4) {
			rgba16_buf[i] = tlut[xform_buf[i+1]];
			rgba16_buf[i+1] = tlut[xform_buf[i+2]];
			rgba16_buf[i+2] = tlut[xform_buf[i+3]];
			rgba16_buf[i+3] = tlut[xform_buf[i+4]];
		}
	}
#endif
//	if (!last_cl_rv) {
		gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
/* 	} else {
		int rv = gfx_opengl_replace_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
		if (rv) {
			gfx_texture_cache_failed((void*)rdp.loaded_texture[tile].addr);
			import_texture(tile);
		}
	} */
}

static void import_texture_ci8(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
//	memset(rgba16_buf, 0, width*height);

//	if (last_set_texture_image_width == 0) {
		uint16_t *tex16 = rgba16_buf;
		uint8_t *tex8 = (uint8_t *)rdp.loaded_texture[tile].addr;
		for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
			*tex16++ = tlut[*tex8++];
		}
#if 0
    } else {
		u32 src_width;
		if (last_set_texture_image_width) {
			src_width = last_set_texture_image_width + 1;
		} else {
			src_width = width;
		}

		uint8_t* start = (uint8_t *)(&rdp.loaded_texture[tile]
							.addr[((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) * last_set_texture_image_width) +
									(rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)]);
		uint16_t *tex16 = rgba16_buf;
		for (uint32_t h = 0; h < height; h ++) {
			uint16_t *tx16 = tex16;
			for (uint32_t w = 0; w < src_width; w+=4) {
				*tx16++ = tlut[*start++];
				*tx16++ = tlut[*start++];
				*tx16++ = tlut[*start++];
				*tx16++ = tlut[*start++];
			}
			tex16 += width;
		}
	}
#endif
//	if (!last_cl_rv) {
		gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
/* 	} else {
		int rv = gfx_opengl_replace_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
		if (rv) {
			gfx_texture_cache_failed((void*)rdp.loaded_texture[tile].addr);
			import_texture(tile);
		}
	} */
}
#else
// enough for a 64 * 64 16-bit image, no overflow should happen at this point
uint16_t __attribute__((aligned(32))) rgba16_buf[4096 * 8];
uint8_t __attribute__((aligned(32))) xform_buf[8192];

int last_cl_rv;

static void __attribute__((noinline)) import_texture(int tile);

static void import_texture_rgba16(int tile) {
	uint32_t i;
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	
	if (last_set_texture_image_width == 0) {
		uint32_t loopcount = rdp.loaded_texture[tile].size_bytes >> 1;
		uint16_t* start = (uint16_t *)rdp.loaded_texture[tile].addr;
		for (i = 0; i < loopcount; i++) {
			uint16_t col16 = __builtin_bswap16(start[i]);
			rgba16_buf[i] = ((col16 & 1) << 15) | (col16 >> 1);
		}
	} else {
		u32 src_width = last_set_texture_image_width + 1;
		uint32_t somewidth = src_width;
		if (width <= ((src_width >> 1) + 4)) {
			somewidth = width;
		} else {
			if (width == 20 && last_set_texture_image_width == 30) { 
				somewidth = width - 4;
			}
		}

		uint16_t* start = (uint16_t*) &rdp.loaded_texture[tile]
					.addr[(((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)) << 1) +
						((((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC)) * (src_width)) << 1)];

		uint16_t *tex16 = rgba16_buf;
		for (i = 0; i < height; i++) {
			for (uint32_t x = 0; x < somewidth; x++) {
				uint16_t col16 = __builtin_bswap16(start[x]);
				*tex16++ = ((col16 & 1) << 15) | (col16 >> 1);
			}
			start += src_width;
		}

		width = somewidth;
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void import_texture_rgba32(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = (rdp.loaded_texture[tile].size_bytes >> 1) / rdp.texture_tile.line_size_bytes;
	uint32_t wxh = width*height;

	// mk64 has one 32-bit texture and it is "Mario Kart 64" on the title screen
	// it only needs one bit of alpha, just make it argb1555
	for (uint32_t i = 0; i < wxh; i++) {
		uint8_t r = rdp.loaded_texture[tile].addr[(4 * i) + 0] >> 3;
		uint8_t g = rdp.loaded_texture[tile].addr[(4 * i) + 1] >> 3;
		uint8_t b = rdp.loaded_texture[tile].addr[(4 * i) + 2] >> 3;
		uint8_t a = !!rdp.loaded_texture[tile].addr[(4 * i) + 3];
		rgba16_buf[i] = (a << 15) | (r << 10) | (g << 5) | (b);
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void import_texture_ia4(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes << 1;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	uint32_t i;

	if (last_set_texture_image_width == 0) {
		for (i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
			uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
			uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
			uint8_t intensity = (SCALE_3_8(part >> 1) >> 3) & 0x1f;
			uint8_t alpha = part & 1;
			uint8_t r = intensity;
			uint8_t g = intensity;
			uint8_t b = intensity;
			uint16_t col16 = (alpha << 15) | (r << 10) | (g << 5) | (b);
			rgba16_buf[i] = col16;
		}
	} else {
		memset(xform_buf, 0, 8192);
		uint8_t* start =
			(uint8_t*) &rdp.loaded_texture[tile]
				.addr[(((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) >> 1) * (last_set_texture_image_width + 1)) +
					  ((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC) >> 1)];
		uint8_t *tex8 = xform_buf;
		for (uint32_t i = 0; i < height; i++) {
			for (uint32_t x = 0; x < (last_set_texture_image_width + 1) * 2; x += 2) {
				uint32_t sidx = x>>1;
				if (i & 1) {
					tex8[(x)] = (start[(sidx)] & 0xf);
					tex8[(x) + 1] = (start[(sidx)] >> 4) & 0xf;

				} else {
					tex8[(x)] = (start[(sidx)] >> 4) & 0xf;
					tex8[(x) + 1] = (start[(sidx)] & 0xf);
				}
			}
			start += (last_set_texture_image_width + 1);
			tex8 += (last_set_texture_image_width + 1);
		}
		for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
			uint8_t byte = xform_buf[i];
			uint8_t intensity = (SCALE_3_8(byte >> 1) >> 3) & 0x1f;
			uint8_t alpha = byte & 1;
			uint8_t r = intensity;
			uint8_t g = intensity;
			uint8_t b = intensity;
			uint16_t col16 = (alpha << 15) | (r << 10) | (g << 5) | (b);
			rgba16_buf[i] = col16;
		}
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static void import_texture_ia8(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	memset(rgba16_buf, 0, width*height*2);//8192*2);

	uint8_t* start = (uint8_t*) &rdp.loaded_texture[tile]
						 .addr[((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) * (last_set_texture_image_width + 1)) +
							   (rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)];

	u32 src_width;
	if (last_set_texture_image_width) {
		src_width = last_set_texture_image_width + 1;
	} else {
		src_width = width;
	}

	uint16_t *tex16 = rgba16_buf;
	for (uint32_t i = 0; i < height; i++) {
		for (uint32_t x = 0; x < src_width; x++) {
			uint8_t val = start[x];
			uint8_t in = ((val >> 4) & 0xf);
			uint8_t al = (val & 0xf);
			tex16[x] = (al << 12) | (in << 8) | (in << 4) | in;
		}
		start += src_width;
		tex16 += src_width;
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void import_texture_ia16(int tile) {
	uint32_t i;
	uint32_t width = rdp.texture_tile.line_size_bytes >> 1;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	u32 src_width;
	if (last_set_texture_image_width) {
		src_width = last_set_texture_image_width + 1;
	} else {
		src_width = width;
	}

	uint16_t* start = rdp.loaded_texture[tile].addr;
	if (last_set_texture_image_width) {
		start =
			(uint16_t*) &rdp.loaded_texture[tile]
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

static void import_texture_i4(int tile) {
	uint32_t i;

	uint32_t width = rdp.texture_tile.line_size_bytes * 2;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

	height = (height + 3) & ~3;

	if (last_set_texture_image_width == 0) {
		for (i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
			uint16_t idx = (i<<1);
			uint8_t byte = rdp.loaded_texture[tile].addr[i];
			uint8_t part1,part2;
			part1 = (byte >> 4) & 0xf;
			part2 = byte & 0xf;
			rgba16_buf[idx  ] = (part1 << 12) | (part1 << 8) | (part1 << 4) | part1;
			rgba16_buf[idx+1] = (part2 << 12) | (part2 << 8) | (part2 << 4) | part2;
		}
	} else {
		memset(rgba16_buf, 0, 8192);
		uint8_t* start =
			(uint8_t*) &rdp.loaded_texture[tile]
				.addr[(((((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC)-1)/2) * (width)/2)) +
					  (((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)-1)/2)];
		for (uint32_t i = 0; i < height; i++) {
			uint32_t iw = i * width;
			for (uint32_t x = 0; x < (last_set_texture_image_width + 1)*2; x += 2) {
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

static void import_texture_i8(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

	memset(xform_buf, 0, width*height*2);

	uint8_t* start = (uint8_t*) &rdp.loaded_texture[tile]
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

	for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
		uint8_t in = (xform_buf[i] >> 4) & 0xf;
		rgba16_buf[i] = (in << 12) | (in << 8) | (in << 4) | in;
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_4_4_4_4_REV);
}

static void import_texture_ci4(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes * 2;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
	uint32_t i;

	if (last_set_texture_image_width == 0) {
		for (i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
			uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
			uint8_t part;
			if (!(i & 1)) {
				part = (byte >> 4) & 0xf;
			} else {
				part = byte & 0xf;
			}
			rgba16_buf[i] = tlut[part];
		}
	} else {
		memset(xform_buf, 0, width * height * 2);
		uint8_t* start =
			(uint8_t*) &rdp.loaded_texture[tile]
				.addr[(((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) / 2) * (last_set_texture_image_width + 1)) +
					  ((rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC) / 2)];
		for (uint32_t i = 0; i < height; i++) {
			for (uint32_t x = 0; x < (last_set_texture_image_width + 1) * 2; x += 2) {
				if (i & 1) {
					xform_buf[(i * (width / 2)) + (x)] = (start[(x / 2)] & 0xf);
					xform_buf[((i * (width / 2)) + (x)) + 1] = (start[(x / 2)] >> 4) & 0xf;

				} else {
					xform_buf[(i * (width / 2)) + (x)] = (start[(x / 2)] >> 4) & 0xf;
					xform_buf[((i * (width / 2)) + (x)) + 1] = (start[(x / 2)] & 0xf);
				}
			}
			start += (last_set_texture_image_width + 1);
		}
		for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i+=4) {
			rgba16_buf[i] = tlut[xform_buf[i+1]];
			rgba16_buf[i+1] = tlut[xform_buf[i+2]];
			rgba16_buf[i+2] = tlut[xform_buf[i+3]];
			rgba16_buf[i+3] = tlut[xform_buf[i+4]];
		}
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}

static __attribute__((noinline)) void import_texture_ci8(int tile) {
	uint32_t width = rdp.texture_tile.line_size_bytes;
	uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

	__builtin_prefetch(tlut);
	if (__builtin_expect((last_set_texture_image_width == 0),1)) {
		uint8_t *tex8 = rdp.loaded_texture[tile].addr;
		if (__builtin_expect((!((uintptr_t)tex8 & 3)),1)) {
			uint32_t *intex32 = (uint32_t *)tex8;
			__builtin_prefetch(intex32);
			uint32_t count = rdp.loaded_texture[tile].size_bytes;
			uint32_t *tex32 = (uint32_t*)rgba16_buf;
			for (uint32_t i = 0; i < count; i+=8) {
				__builtin_prefetch((void*)(((uintptr_t)intex32&0xffffffe0)+32));

				uint32_t fourpix1 = *intex32++;
				uint32_t fourpix2 = *intex32++;

				asm volatile("": : : "memory");

				uint16_t t1,t2,t3,t4,t5,t6,t7,t8;

				t4 = tlut[(fourpix1 >> 24) & 0xff];
				t3 = tlut[(fourpix1 >> 16) & 0xff];
				t2 = tlut[(fourpix1 >>  8) & 0xff];
				t1 = tlut[(fourpix1      ) & 0xff];
				t8 = tlut[(fourpix2 >> 24) & 0xff];
				t7 = tlut[(fourpix2 >> 16) & 0xff];
				t6 = tlut[(fourpix2 >>  8) & 0xff];
				t5 = tlut[(fourpix2      ) & 0xff];

				asm volatile("": : : "memory");

				*tex32++ = (t2 << 16) | t1;
				*tex32++ = (t4 << 16) | t3;
				*tex32++ = (t6 << 16) | t5;
				*tex32++ = (t8 << 16) | t7;
			}
		} else {
			__builtin_prefetch(tex8);
			uint32_t count = rdp.loaded_texture[tile].size_bytes;
			uint32_t *tex32 = (uint32_t*)rgba16_buf;
			for (uint32_t i = 0; i < count; i+=8) {
				uint16_t t1,t2,t3,t4,t5,t6,t7,t8;
				__builtin_prefetch(tex8+16);
				t1 = tlut[*tex8++];
				t2 = tlut[*tex8++];
				t3 = tlut[*tex8++];
				t4 = tlut[*tex8++];
				t5 = tlut[*tex8++];
				t6 = tlut[*tex8++];
				t7 = tlut[*tex8++];
				t8 = tlut[*tex8++];
	#if 1
				asm volatile("": : : "memory");
	#endif

				*tex32++ = (t2 << 16) | t1;
				*tex32++ = (t4 << 16) | t3;
				*tex32++ = (t6 << 16) | t5;
				*tex32++ = (t8 << 16) | t7;
			}
		}
	} else {
		u32 src_width;
		if (last_set_texture_image_width) {
			src_width = last_set_texture_image_width + 1;
		} else {
			src_width = width;
		}

		uint8_t* start = (uint8_t *)(&rdp.loaded_texture[tile]
							.addr[((rdp.texture_tile.ult >> G_TEXTURE_IMAGE_FRAC) * last_set_texture_image_width) +
									(rdp.texture_tile.uls >> G_TEXTURE_IMAGE_FRAC)]);
//		uint32_t *start32;
		uint16_t *tex16 = rgba16_buf;
		for (uint32_t h = 0; h < height; h ++) {
			//uint16_t *tx16 = tex16;
			uint16_t *tx32 = tex16;
			for (uint32_t w = 0; w < src_width; w += 4) {
				uint16_t t1,t2,t3,t4;
				t1 = tlut[*start++];
				t2 = tlut[*start++];
				t3 = tlut[*start++];
				t4 = tlut[*start++];

				*tx32++ = (t2 << 16) | t1;
				*tx32++ = (t4 << 16) | t3;
				//				*tx16++ = t2;
//				*tx16++ = t3;
//				*tx16++ = t4;
			}
			tex16 += width;
		}
	}

	gfx_rapi->upload_texture((uint8_t*) rgba16_buf, width, height, GL_UNSIGNED_SHORT_1_5_5_5_REV);
}
#endif
static void import_texture(int tile) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
    
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], rdp.loaded_texture[tile].addr, fmt, siz)) {
        return;
    }
    
    //int t0 = get_time();
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    //int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);
}
//#include <kos.h>
#include "sh4zam.h"

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

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
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
//    memcpy(matrix, addr, sizeof(matrix));
        shz_xmtrx_load_4x4_unaligned((const float*)addr);
        shz_xmtrx_store_4x4((shz_matrix_4x4_t *)matrix);
#endif
    
    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            n64_memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul((shz_matrix_4x4_t *)rsp.P_matrix, (const shz_matrix_4x4_t *)matrix, (const shz_matrix_4x4_t *)rsp.P_matrix);
        }
//        glMatrixMode(GL_PROJECTION);
  //      glLoadMatrixf((const float*)rsp.P_matrix);
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            n64_memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            n64_memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul((shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)matrix, (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
    //    glMatrixMode(GL_MODELVIEW);
      //  glLoadMatrixf((const float*)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        rsp.lights_changed = 1;
    }
    matrix_dirty = 1;
    gfx_matrix_mul((shz_matrix_4x4_t *)rsp.MP_matrix, (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
        matrix_dirty = 1;
        gfx_matrix_mul((shz_matrix_4x4_t *)rsp.MP_matrix, (const shz_matrix_4x4_t *)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], (const shz_matrix_4x4_t *)rsp.P_matrix);
//        glMatrixMode(GL_MODELVIEW);
  //      glLoadMatrixf((const float*)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / ((float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height);
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

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    size_t i;
    shz_xmtrx_load_4x4((const shz_matrix_4x4_t *)&rsp.MP_matrix);
    if (rsp.geometry_mode & G_LIGHTING) {
        if (rsp.lights_changed) {
            int i;
            for (i = 0; i < rsp.current_num_lights - 1; i++) {
                calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
            }
            static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
            static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
            calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
            calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
            rsp.lights_changed = false;
        }
    }

	for (i = 0; i < n_vertices; i++, dest_index++) {
		const Vtx_t* v = &vertices[i].v;
		const Vtx_tn* vn = &vertices[i].n;
		struct LoadedVertex* d = &rsp.loaded_vertices[dest_index];

        shz_vec4_t out = shz_xmtrx_trans_vec4(shz_vec3_vec4(shz_vec3_deref(v->ob), 1.0f));
            //(shz_vec4_t) { .x = v->ob[0], .y = v->ob[1], .z = v->ob[2], .w = 1.0f });
        MEM_BARRIER_PREF(v + 1);
        d->x = v->ob[0];
        d->y = v->ob[1];
        d->z = v->ob[2];

        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        d->color.a = v->cn[3];

        MEM_BARRIER();
        float x, y, z, w;
        x = out.x;
        y = out.y;
        z = out.z;
        w = out.w;
        MEM_BARRIER();

        float recw = shz_fast_invf(w);

        if (rsp.geometry_mode & G_LIGHTING) {
            /* if (rsp.lights_changed) {
                int i;
                for (i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lights_changed = false;
            } */
            
            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
            int i;

            for (i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = MAX((float)0.0f, (float)(recip127*shz_dot6f((float)vn->n[0], (float)vn->n[1], (float)vn->n[2],
                rsp.current_lights_coeffs[i][0], rsp.current_lights_coeffs[i][1],rsp.current_lights_coeffs[i][2])));

                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }

#if 0
//            d->color.r = r > 255 ? 255 : r;
 //           d->color.g = g > 255 ? 255 : g;
 //           d->color.b = b > 255 ? 255 : b;
            float max_c = MAX4(255.0f, (float)r, (float)g, (float)b);
            float maxc = shz_div_posf(255.0f, (float) max_c);

            d->color.r = (uint8_t) ((float)r * maxc);
            d->color.g = (uint8_t) ((float)g * maxc);
            d->color.b = (uint8_t) ((float)b * maxc);
#endif
            d->color.r = MIN(255,r);
            d->color.g = MIN(255,g);
            d->color.b = MIN(255,b);
            #define recip2pi 0.159155f

            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx; // = 0,
                float doty; // = 0;
                {
                    register float fr8  asm ("fr8")  = vn->n[0];
                    register float fr9  asm ("fr9")  = vn->n[1];
                    register float fr10 asm ("fr10") = vn->n[2];
                    register float fr11 asm ("fr11") = 0;
    
                    dotx = recip127 * shz_dot8f(fr8, fr9, fr10, fr11, rsp.current_lookat_coeffs[0][0],
                                                rsp.current_lookat_coeffs[0][1], rsp.current_lookat_coeffs[0][2], 0);

                    doty = recip127 * shz_dot8f(fr8, fr9, fr10, fr11, rsp.current_lookat_coeffs[1][0],
                                                rsp.current_lookat_coeffs[1][1], rsp.current_lookat_coeffs[1][2], 0);
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
                    dotx = acosf(/* - */dotx) * recip2pi;
                    doty = acosf(/* - */doty) * recip2pi;
                } else {
                    dotx = (dotx * 0.25f) + 0.25f; ////1.0f) / 4.0f;
                    doty = (doty * 0.25f) + 0.25f; // 1.0f) / 4.0f;
                }

                U = (int32_t) (dotx * rsp.texture_scaling_factor.s);
                V = (int32_t) (doty * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;
        
        // trivial clip rejection
        uint8_t cr = ((rsp.geometry_mode & G_LIGHTING) ? 128 : 0) | ((w < 0) ? 64 : 0x00);
        clip_rej[dest_index] = cr | trivial_reject(x, y, z, w);
        d->_x = x * recw;
        d->_y = y * recw;
    }
}

//							const uint8_t frac = (uint8_t) (distance_frac * 255.0f);
//							buf_vbo[buf_num_vert].color.packed = PACK_ARGB8888(frac, frac, frac, frac);
//							break;
uint8_t trilerp_a = 0;

extern void get_mario_pos(float *x, float *y, float *z);
/* float late_ramp(float x) {
    float t = (x - 0.7f) / 0.3f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float n = 4.0f;                  // tweak 4..10
    return 0.25f + 0.75f * powf(t, n);
} */
static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

#if 0
float late_ramp(float x) {
    float t = clamp01((x - 0.905f) / 0.1f);   // 0 until x=0.9, 1 at x=1.0
    float n = 8.0f;                         // tweak 3..10
    return 0.25f + 0.75f * powf(t, n);
}
#endif
#if 0
float smootherstep(float t) {
    return t*t*t*(t*(t*6.0f - 15.0f) + 10.0f);
}

float late_ramp(float x) {
    float t = clamp01((x - 0.9f) / 0.1f);
    float s = smootherstep(t);
    float k = 2.0f;              // 1..4
    return 0.25f + 0.75f * powf(s, k);
}
#endif

float step_ramp_pow(float x, float param, float n) {
    if (param >= 1.0f) return 0.0f;          // degenerate: never ramps
    if (param <= 0.0f) param = 0.0f;

    float t = clamp01((x - param) / (1.0f - param));
    return powf(t, n);
}

int eyeball_guy = 0;
static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
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

    if (matrix_dirty) {
        gfx_flush();
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf((const float*) rsp.MP_matrix);
        matrix_dirty = 0;
    }

    if ((c0 & c1 & c2) & 0x3f) {
        // The whole triangle lies outside the visible area
        return;
    }
    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float dx1 = v1->_x - v2->_x;
        float dy1 = v1->_y - v2->_y;
        float dx2 = v3->_x - v2->_x;
        float dy2 = v3->_y - v2->_y;
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

    if (in_trilerp) {
        if (doing_peach && in_trilerp == 1) {
            float mx,my,mz;
            get_mario_pos(&mx,&my,&mz);
            float distance_frac = mz / -4000.0f;

            if (distance_frac < 0.0f)
				distance_frac = 0.0f;
			if (distance_frac > 1.0f)
    			distance_frac = 1.0f;

            distance_frac = 1.0f - step_ramp_pow(distance_frac, 0.8f, 2.0f);

            trilerp_a = (uint8_t)(distance_frac * 255.0f);
        }
    }

    if (do_radar_mark)
        gfx_flush();
//    if (do_radar_mark) {
//        screen_2d_z += 1.0f;
//    }

    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;

    if (water_bomb) depth_test = 0;

    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    uint32_t cc_id = rdp.combine_mode;
    
    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    if ((rsp.use_fog != use_fog)) {
        gfx_flush();
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
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }

    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }

    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    int i;

    uint8_t usetex = used_textures[0];
    uint8_t linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;

    if (usetex) {
        if (rdp.textures_changed[0]) {
            // necessary
            gfx_flush();
            import_texture(0);
            rdp.textures_changed[0] = 0;
        }
        uint8_t cms = rdp.texture_tile.cms;
        uint8_t cmt = rdp.texture_tile.cmt;

        uint32_t tex_size_bytes = rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes;
        uint32_t line_size = rdp.texture_tile.line_size_bytes;
        uint32_t tex_height_i;
        if (line_size == 0) {
            line_size = 1;
            tex_height_i = tex_size_bytes;
        } else {
            tex_height_i = (uint32_t) ((float) tex_size_bytes / (float) line_size);
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

        if (((linear_filter != rendering_state.textures[0]->linear_filter) || (rendering_state.textures[0]->cms != cms) || (rendering_state.textures[0]->cmt != cmt))) {
            gfx_flush();
        }

        gfx_rapi->set_sampler_parameters(0, linear_filter, cms, cmt);
        rendering_state.textures[0]->linear_filter = linear_filter;
        rendering_state.textures[0]->cms = cms;
        rendering_state.textures[0]->cmt = cmt;
    }

    bool use_texture = used_textures[0] || used_textures[1];
    uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;
    uint8_t lit = l_clip_rej[0] & 0x80;

    uint32_t color_r, color_g, color_b, color_a;
    uint32_t cc_rgb = rdp.combine_mode & 0xFFF;
    uint32_t packedc;
    color_r = color_g = color_b = color_a = 255;
    int use_shade = 0;

    if (cc_rgb == 0x0c1) {
        lit = 0;
        color_r = rdp.prim_color.r;
        color_g = rdp.prim_color.g;
        color_b = rdp.prim_color.b;
        color_a = rdp.prim_color.a;
    } else if (cc_rgb == 0x200) {
        lit = 0;
        color_r = 255;
        color_g = 255;
        color_b = 255;
        color_a = 255;
    } else if (doing_text_bg_box) {
        color_r = rdp.env_color.r;
        color_g = rdp.env_color.g;
        color_b = rdp.env_color.b;
        color_a = rdp.env_color.a;
        lit = 0;
    } else if (cc_rgb == 0x668) {
        // (0 - G_CCMUX_ENV) * 1 + G_CCMUX_PRIM
        color_r = (rdp.prim_color.r - rdp.env_color.r);
        color_g = (rdp.prim_color.g - rdp.env_color.g);
        color_b = (rdp.prim_color.b - rdp.env_color.b);
        color_a = rdp.prim_color.a;
    } else if (num_inputs > 1) {
        int i0 = comb->shader_input_mapping[0][1] == CC_PRIM;
        int i2 = comb->shader_input_mapping[0][0] == CC_ENV;

        int i3 = comb->shader_input_mapping[0][0] == CC_PRIM;
        int i4 = comb->shader_input_mapping[0][1] == CC_ENV;

        if (i0 && i2) {
            color_r = 255 - rdp.env_color.r;
            color_g = 255 - rdp.env_color.g;
            color_b = 255 - rdp.env_color.b;
            color_a = rdp.prim_color.a;
        } else if (i3 && i4) {
            color_r = rdp.prim_color.r;
            color_g = rdp.prim_color.g;
            color_b = rdp.prim_color.b;
            color_a = rdp.prim_color.a;

            color_r *= ((rdp.env_color.r + 255));
            color_g *= ((rdp.env_color.g + 255));
            color_b *= ((rdp.env_color.b + 255));
            color_a *= rdp.env_color.a;

            color_r >>= 8;
            color_g >>= 8;
            color_b >>= 8;
            color_a >>= 8;

            uint32_t max_c;
            max_c = MAX4(255, color_r, color_g, color_b);
            float maxc = shz_div_posf(255.0f, (float) max_c);

            float rn, gn, bn;
            rn = (float) color_r * maxc;
            gn = (float) color_g * maxc;
            bn = (float) color_b * maxc;
            color_r = (uint32_t) rn;
            color_g = (uint32_t) gn;
            color_b = (uint32_t) bn;
        } else {
            goto othercolorcode;
        }
    } else {
    othercolorcode:
        int k;
        for (k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
            switch (comb->shader_input_mapping[k][0]) {
                case G_CCMUX_PRIMITIVE_ALPHA:
                    color_r = color_g = color_b = rdp.prim_color.a;
                    color_a = k ? rdp.prim_color.a : 255;
                    use_shade = 0;
                    break;
                case G_CCMUX_ENV_ALPHA:
                    color_r = color_g = color_b = rdp.env_color.a;
                    color_a = k ? rdp.env_color.a : 255;
                    use_shade = 0;
                    break;
                case CC_PRIM:
                    color_r = rdp.prim_color.r;
                    color_g = rdp.prim_color.g;
                    color_b = rdp.prim_color.b;
                    color_a = k ? rdp.prim_color.a : 255;
                    use_shade = 0;
                    break;
                case CC_SHADE:
                    use_shade = 1 + k;
                    break;
                case CC_ENV:
                    color_r = rdp.env_color.r;
                    color_g = rdp.env_color.g;
                    color_b = rdp.env_color.b;
                    color_a = k ? rdp.env_color.a : 255;
                    use_shade = 0;
                    break;
                default:
                    color_a = 255;
//                    if (!lit) {
                        color_r = color_g = color_b = 255;
//                    }
                    use_shade = 0;
                    break;
            }
        }
    }
    float recip_tw = shz_fast_invf(tex_width);
    float recip_th = shz_fast_invf(tex_height);

    packedc = PACK_ARGB8888(color_r, color_g, color_b, color_a);

    for (i = 0; i < 3; i++) {
        if (do_radar_mark) {
            buf_vbo[buf_num_vert].vert.x = (v_arr[i]->_x * SCREEN_WIDTH) + SCREEN_WIDTH;
            buf_vbo[buf_num_vert].vert.y = SCREEN_HEIGHT - (v_arr[i]->_y * SCREEN_HEIGHT);
            buf_vbo[buf_num_vert].vert.z = 10000.0f;
        } else {
            buf_vbo[buf_num_vert].vert.x = v_arr[i]->x;
            buf_vbo[buf_num_vert].vert.y = v_arr[i]->y;
            buf_vbo[buf_num_vert].vert.z = v_arr[i]->z;
        }
        //buf_vbo[buf_vbo_len++] = z;
        
        if (use_texture) {
            float u = (v_arr[i]->u - (rdp.texture_tile.uls << 3)) * 0.03125f;// / 32.0f;
            float v = (v_arr[i]->v - (rdp.texture_tile.ult << 3)) * 0.03125f;// / 32.0f;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            buf_vbo[buf_num_vert].texture.u = u * recip_tw; // / tex_width;
            buf_vbo[buf_num_vert].texture.v = v * recip_th; // / tex_height;
        }

        if (use_shade) {
            color_r = v_arr[i]->color.r;
            color_g = v_arr[i]->color.g;
            color_b = v_arr[i]->color.b;
            color_a = (use_shade - 1) ? v_arr[i]->color.a : 255;
            packedc = PACK_ARGB8888(color_r, color_g, color_b, color_a);
        } else if (lit) {
            uint32_t tc_r;
            uint32_t tc_g;
            uint32_t tc_b;
            uint32_t light_r = v_arr[i]->color.r;
            uint32_t light_g = v_arr[i]->color.g;
            uint32_t light_b = v_arr[i]->color.b;
                tc_r = ((((255 + color_r) >> 1) * light_r) >> 8) & 0xff;
                tc_g = ((((255 + color_g) >> 1) * light_g) >> 8) & 0xff;
                tc_b = ((((255 + color_b) >> 1) * light_b) >> 8) & 0xff;
            packedc = PACK_ARGB8888(tc_r, tc_g, tc_b, color_a);
        }

        buf_vbo[buf_num_vert].color.packed = packedc;

		buf_num_vert++;

        buf_vbo_len += sizeof(dc_fast_t);
	}

    buf_vbo_num_tris += 1;

    if (do_radar_mark || drawing_hand || in_trilerp || doing_skybox || water_bomb || aquarium_draw || buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

extern void gfx_opengl_draw_triangles_2d(void *buf_vbo, size_t buf_vbo_len, size_t buf_vbo_num_tris);
#if 0
static void gfx_sp_quad_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, uint8_t vtx1_idx2, uint8_t vtx2_idx2, uint8_t vtx3_idx2) {
    gfx_flush();
    dc_fast_t *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    dc_fast_t *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    dc_fast_t *v3 = &rsp.loaded_vertices_2D[vtx3_idx];

    dc_fast_t *v12 = &rsp.loaded_vertices_2D[vtx1_idx2];
    dc_fast_t *v22 = &rsp.loaded_vertices_2D[vtx2_idx2];
    dc_fast_t *v32 = &rsp.loaded_vertices_2D[vtx3_idx2];

    dc_fast_t *v_arr[6] = {v1, v2, v3, v12, v22, v32};

    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    uint32_t cc_id = rdp.combine_mode;
    
    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
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
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }

    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    int i;

    for (i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
            if (linear_filter != rendering_state.textures[i]->linear_filter || rdp.texture_tile.cms != rendering_state.textures[i]->cms || rdp.texture_tile.cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile.cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile.cmt;
            }
        }
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

    //float tri_buf[10*6] = {0};
    int tri_num_vert = 0;
    for (tri_num_vert = 0; tri_num_vert < 6; tri_num_vert++) {
        quad_vbo[tri_num_vert].vert.x = v_arr[tri_num_vert]->vert.x;
        quad_vbo[tri_num_vert].vert.y = v_arr[tri_num_vert]->vert.y;
        quad_vbo[tri_num_vert].vert.z = v_arr[tri_num_vert]->vert.z;
        
        if (use_texture) {
            float u = (v_arr[tri_num_vert]->texture.u - rdp.texture_tile.uls * 8) / 32.0f;
            float v = (v_arr[tri_num_vert]->texture.v - rdp.texture_tile.ult * 8) / 32.0f;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            quad_vbo[tri_num_vert].texture.u = u / tex_width;
            quad_vbo[tri_num_vert].texture.v = v / tex_height;
        }

        struct RGBA white = (struct RGBA){0xff, 0xff, 0xff, 0xff};
        struct RGBA *color = &white;
        //struct RGBA tmp;
        int j, k;

        for (j = 0; j < num_inputs; j++) {
            /*@Note: use_alpha ? 1 : 0 */
            for (k = 0; k < 1 + (use_alpha ? 0 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                    case CC_PRIM:
                        color = &rdp.prim_color;
                        break;
                    case CC_SHADE:
                        color = (struct RGBA *)&v_arr[tri_num_vert]->color;
                        break;
                    case CC_ENV:
                        color = &rdp.env_color;
                        break;
                    default:
                        //memset(&tmp, 0, sizeof(tmp));
                        //color = &tmp;
                        color = &white;
                        break;
                }
            }
        }
        quad_vbo[tri_num_vert].color.array.r = color->r;
        quad_vbo[tri_num_vert].color.array.g = color->g;
        quad_vbo[tri_num_vert].color.array.b = color->b;
        quad_vbo[tri_num_vert].color.array.a = color->a;
    }
    gfx_opengl_draw_triangles_2d((void*)quad_vbo, 0, use_texture);
}
#endif
int do_ext_fill = 0;

static void __attribute__((noinline)) gfx_sp_quad_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx,
                                                     uint8_t vtx1_idx2, uint8_t vtx2_idx2, uint8_t vtx3_idx2) {
    dc_fast_t* v2d = &rsp.loaded_vertices_2D[0];
    gfx_flush();
    uint8_t depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    uint8_t z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
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
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
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
    uint8_t used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    uint8_t linear_filter = 1;

    if (used_textures[0]) {
        if (rdp.textures_changed[0]) {
            import_texture(0);
            rdp.textures_changed[0] = 0;
        }
        linear_filter =  ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT);
        gfx_rapi->set_sampler_parameters(0, linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
        rendering_state.textures[0]->linear_filter = linear_filter;
        rendering_state.textures[0]->cms = rdp.texture_tile.cms;
        rendering_state.textures[0]->cmt = rdp.texture_tile.cmt;
    }

    uint8_t use_texture = !do_ext_fill && used_textures[0];

    dc_fast_t* tmpv = v2d;

    if (use_texture) {
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
            tmpv->texture.u = u * recip_tex_width ;
            v = (tmpv->texture.v * 0.03125f) - ult;
            tmpv++->texture.v = v * recip_tex_height;

            u = (tmpv->texture.u * 0.03125f) - uls;
            tmpv->texture.u = u * recip_tex_width ;
            v = (tmpv->texture.v * 0.03125f) - ult;
            tmpv->texture.v = v * recip_tex_height ;
        
    }

    uint32_t rectcolor = 0xffffffff;
    uint32_t color_r = 0;
    uint32_t color_g = 0;
    uint32_t color_b = 0;
    uint32_t color_a = 0;
#if 0
    if (num_inputs > 1) {
        int i0 = comb->shader_input_mapping[0][1] == CC_PRIM;
        int i2 = comb->shader_input_mapping[0][0] == CC_ENV;

        int i3 = comb->shader_input_mapping[0][0] == CC_PRIM;
        int i4 = comb->shader_input_mapping[0][1] == CC_ENV;

        if (i0 && i2) {
            color_r = 255 - rdp.env_color.r;
            color_g = 255 - rdp.env_color.g;
            color_b = 255 - rdp.env_color.b;
            color_a = rdp.prim_color.a;

            rectcolor = PACK_ARGB8888(color_r, color_g, color_b, color_a);
        } else if (i3 && i4) {
            color_r = rdp.prim_color.r;
            color_g = rdp.prim_color.g;
            color_b = rdp.prim_color.b;
            color_a = rdp.prim_color.a;

            color_r *= ((rdp.env_color.r + 255));
            color_g *= ((rdp.env_color.g + 255));
            color_b *= ((rdp.env_color.b + 255));
            color_a *= rdp.env_color.a;

            color_r >>= 8;
            color_g >>= 8;
            color_b >>= 8;
            color_a >>= 8;

            float rn, gn, bn;
            uint32_t max_c = MAX4(255, color_r, color_g, color_b);
            float maxc = shz_div_posf(255.0f, (float) max_c);

            rn = (float) color_r * maxc;
            gn = (float) color_g * maxc;
            bn = (float) color_b * maxc;
            color_r = (uint32_t) rn;
            color_g = (uint32_t) gn;
            color_b = (uint32_t) bn;

            rectcolor = PACK_ARGB8888(color_r, color_g, color_b, color_a);
        } else {
            goto thenext2dthing;
        }
    } else 
#endif     
    {
//    thenext2dthing:
        int k;
        for (k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
            switch (comb->shader_input_mapping[k][0]) {
                case CC_PRIM:
                    rectcolor = PACK_ARGB8888(rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, k ? rdp.prim_color.a : 255);
                    break;
                case CC_SHADE:
//                    if (!use_alpha)
//                        v2d[0].color.array.a = 255;
//                    else 
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
    }

    v2d++->color.packed = rectcolor;
    v2d++->color.packed = rectcolor;
    v2d++->color.packed = rectcolor;
    v2d->color.packed = rectcolor;

    gfx_opengl_draw_triangles_2d((void*) rsp.loaded_vertices_2D, 4, use_texture);
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);
    
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

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
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


extern float gl_fog_start;
extern float gl_fog_end;
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

    const float t = x * 0.001f;
    const float num = expm1f(a * (t - 1.0f));
    const float den = expm1f(-a);
    return 1000.0f * (1.0f - shz_divf(num, den));
}


static void gfx_sp_moveword(uint8_t index, UNUSED uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            if ((!rendering_state.fog_change)) {
                rendering_state.fog_change = 1;
            float recip_fog_mul = shz_fast_invf(rsp.fog_mul);
            float n64_min = 500.0f * (1.0f - (float) rsp.fog_offset * recip_fog_mul);
            float n64_max = n64_min + 128000.0f * recip_fog_mul;
            float scale = /*( gProjectFar - gProjectNear )*/24000.0f * 0.001f;
            gl_fog_start = /* gProjectNear  */10.0f + scale * exp_map_0_1000_f(n64_min);
            gl_fog_end = /* gProjectNear  */10.0f  + scale * exp_map_0_1000_f(n64_max);
                //fog_dirty = 1;
            }
                            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, UNUSED uint8_t level, UNUSED uint8_t tile, UNUSED uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

static void gfx_dp_set_scissor(UNUSED uint32_t mode, uint32_t ulx, uint32_t uly, UNUSED uint32_t lrx, UNUSED uint32_t lry) {
    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(UNUSED uint32_t format, uint32_t size,  uint32_t width,  const void* addr) {
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
		last_set_texture_image_width = width;

}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile,  uint32_t palette, uint32_t cmt,  uint32_t maskt,  uint32_t shiftt, uint32_t cms,  uint32_t masks,  uint32_t shifts) {
    if (tile == G_TX_RENDERTILE) {
        SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
        rdp.texture_tile.line_size_bytes = line * 8;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    
    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, UNUSED uint32_t high_index) {
//    SUPPORT_CHECK(tile == G_TX_LOADTILE);
  //  SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
    uint16_t *p = (uint16_t *)rdp.palette;
    for (int i=0;i<256;i++) {
		uint16_t col16 = p[i];
        col16 = (col16 << 8) | ((col16 >> 8) & 0xff);
        tlut[i] = ((col16 & 1) << 15) | (col16 >> 1);
    }
}

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, UNUSED uint32_t dxt) {
//    if (tile == 1) return;
  //  SUPPORT_CHECK(tile == G_TX_LOADTILE);
    //SUPPORT_CHECK(uls == 0);
    //SUPPORT_CHECK(ult == 0);
    
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
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
 //   assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
  //  if (tile == 1) return;
    //SUPPORT_CHECK(tile == G_TX_LOADTILE);
    //SUPPORT_CHECK(uls == 0);
    //SUPPORT_CHECK(ult == 0);

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
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

   // assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;

    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
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

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

#define recip255 0.00392157f
static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
    if ((!rendering_state.fog_col_change)) {
        rendering_state.fog_col_change = 1;

        float fog_color[4] = { rdp.fog_color.r * recip255, rdp.fog_color.g * recip255, rdp.fog_color.b * recip255,
                               1.0f };
        glFogfv(GL_FOG_COLOR, fog_color);
    }
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

void gfx_opengl_2d_projection(void) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 640, 480, 0, -10000, 10000);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    matrix_dirty = 1;
}

void gfx_opengl_reset_projection(void) {
//    glMatrixMode(GL_PROJECTION);
  //  glLoadMatrixf((const float*)rsp.MP_matrix);
    //glMatrixMode(GL_MODELVIEW);
    //glLoadIdentity();
    //    glLoadMatrixf((const float*)rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    matrix_dirty = 1;

}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
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

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = (ulyf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = (lryf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
    
    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    ulxf = (ulxf*320)+320;
    lrxf = (lrxf*320)+320;

    ulyf = (ulyf*240)+240;
    lryf = (lryf*240)+240;
    
    dc_fast_t *ul = &rsp.loaded_vertices_2D[0];
    dc_fast_t *ll = &rsp.loaded_vertices_2D[1];
    dc_fast_t *lr = &rsp.loaded_vertices_2D[2];
    dc_fast_t *ur = &rsp.loaded_vertices_2D[3];

    ul->vert.x = ulxf;
    ul->vert.y = ulyf;
    ul->vert.z = 1.0f;

    ll->vert.x = ulxf;
    ll->vert.y = lryf;
    ll->vert.z = 1.0f;

    lr->vert.x = lrxf;
    lr->vert.y = lryf;
    lr->vert.z = 1.0f;

    ur->vert.x = lrxf;
    ur->vert.y = ulyf;
    ur->vert.z = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    gfx_sp_quad_2d(0, 1, 3, 1, 2, 3);

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, UNUSED uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
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
    
    dc_fast_t *ul = &rsp.loaded_vertices_2D[0];
    dc_fast_t *ll = &rsp.loaded_vertices_2D[1];
    dc_fast_t *lr = &rsp.loaded_vertices_2D[2];
    dc_fast_t *ur = &rsp.loaded_vertices_2D[3];
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

    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
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
    
//    uint32_t saved_combine_mode = rdp.combine_mode;
  //  gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE));
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    //rdp.combine_mode = saved_combine_mode;
    rsp.geometry_mode = saved_geom_mode;
    rdp.other_mode_l = saved_other_mode_l;
    do_ext_fill = 0;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(UNUSED uint32_t format, UNUSED uint32_t size, UNUSED uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

//static inline void *seg_addr(uintptr_t w1) {
//    return (void *) w1;
//}

#define seg_addr(a) ((void*)a)

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))
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

static void gfx_run_dl(Gfx* cmd) {
//    if (cmd == dl_menu_idle_hand || cmd == dl_menu_grabbing_hand) {
  //      drawing_hand = 1;
   // } else {
    //    drawing_hand = 0;
    //}
    drawing_hand = 0;
    if (cmd == mr_i_eyeball_seg6_dl_06002080) {
        eyeball_guy = 1;
    } else {
        eyeball_guy = 0;
    }

    if (cmd == water_bubble_seg5_dl_05010D30) {
        water_bomb = 1;
    } else {
        water_bomb = 0;
    }

    if (cmd == dl_draw_text_bg_box) {
        doing_text_bg_box = 1;
    } else {
        doing_text_bg_box = 0;
    }

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        if (cmd == dl_menu_hand) {
            drawing_hand = 1;
        }
        if (cmd == dl_ia_text_begin) {
            font_draw = 1;
        }
        if (cmd == dl_ia_text_end) {
            font_draw = 0;
        }

        if (cmd->words.w0 == 0x424C4E44) {
                if (cmd->words.w1 == 0x87654321) {
                    do_radar_mark ^= 1;
                }
                if (cmd->words.w1 == 0x12345678) {
                    doing_skybox ^= 1;
                }
                else if (cmd->words.w1 == 0xAAAAAAAA) {
                    in_trilerp = 1;
                    doing_peach = 0;
                    doing_bowser = 0;
                }
                else if (cmd->words.w1 == 0xBBBBBBBB) {
                    doing_peach = 1;
                }
                else if (cmd->words.w1 == 0xCCCCCCCC) {
                    doing_bowser = 1;
                    doing_peach = 0;
                }
                else if(cmd->words.w1 == 0xDDDDDDDD) {
                    in_trilerp = 0;
                    doing_bowser = 0;
                    doing_peach = 0;
                }
        }

        switch (opcode) {
            case G_RDPPIPESYNC:
                gfx_flush();
                break;

            // RSP commands:
            case G_MTX:
//                gfx_flush();
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
//                gfx_flush();
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
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
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
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
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
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
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
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
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
                    /*color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                    color_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));*/
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
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
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
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
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
    rendering_state.textures[0]->cms = 6;
    rendering_state.textures[0]->cmt = 6;
    rendering_state.textures[1]->cms = 6;
    rendering_state.textures[1]->cmt = 6;

rendering_state.fog_col_change = 0;
rendering_state.fog_change = 0;
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
#if 0
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
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

void gfx_start_frame(void) {
    gfx_wapi->handle_events();
    first_2d_in_frame = 0;
}

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
    
    //puts("New frame");
    
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;
    
    //double t0 = gfx_wapi->get_time();
    gfx_rapi->start_frame();
    gfx_run_dl(commands);
    gfx_flush();
    //double t1 = gfx_wapi->get_time();
    //printf("Process %f %f\n", t1, t1 - t0);
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
}

void gfx_end_frame(void) {
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
}
#endif
