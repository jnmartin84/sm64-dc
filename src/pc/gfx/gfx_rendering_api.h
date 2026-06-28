#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H

#include <stddef.h>
#include <stdint.h>
//#include <stdbool.h>
#undef bool
#define bool uint8_t

// Abstract blend factors. The front-end derives these from the N64 blender (other_mode_l,
// game-agnostic) and the raw-PVR backend maps them onto PVR blend modes. Keeps N64 knowledge
// in the front-end and GPU knowledge in the backend — portable across games and backends.
enum gfx_blend_factor {
    GFX_BLENDF_ZERO = 0,
    GFX_BLENDF_ONE,
    GFX_BLENDF_SRCALPHA,
    GFX_BLENDF_INVSRCALPHA,
    GFX_BLENDF_DSTALPHA,
    GFX_BLENDF_INVDSTALPHA,
};

// Abstract texture-environment mode, derived in the front-end from the N64 color combiner and
// mapped onto the backend's texel<->vertex-color combine. Values intentionally match the PVR
// pvr_txr_shading_mode enum order so the raw-PVR backend maps 1:1. (See texenv derivation notes.)
enum gfx_tex_env {
    GFX_TEXENV_REPLACE = 0,        // px = tex                       (DECALRGBA)
    GFX_TEXENV_MODULATE,           // rgb = col*tex, a = tex.a       (MODULATEIDECALA, texel-free+oargb)
    GFX_TEXENV_DECAL,              // rgb = lerp(col,tex,tex.a), a=col.a  (DECALRGB, BLENDRGBFADEA)
    GFX_TEXENV_MODULATEALPHA,      // rgb = col*tex, a = col.a*tex.a  (MODULATEI/IA)
};

struct ShaderProgram;

struct GfxRenderingAPI {
    bool (*z_is_from_0_to_1)(void);
    void (*unload_shader)(struct ShaderProgram *old_prg);
    void (*load_shader)(struct ShaderProgram *new_prg);
    struct ShaderProgram *(*create_and_load_new_shader)(uint32_t shader_id);
    struct ShaderProgram *(*lookup_shader)(uint32_t shader_id);
    bool (*shader_get_info)(struct ShaderProgram *prg, uint8_t *num_inputs);
    uint32_t (*new_texture)(void);
    void (*select_texture)(uint32_t texture_id);
    void (*upload_texture)(const uint8_t *rgba32_buf, int width, int height, unsigned int type);
    void (*set_sampler_parameters)(bool linear_filter, uint32_t cms, uint32_t cmt);
    void (*set_depth_test)(bool depth_test);
    void (*set_depth_mask)(bool z_upd);
    void (*set_zmode_decal)(bool zmode_decal);
    // Texel<->vertex-color combine (enum gfx_tex_env), derived from the N64 combiner. Raw-PVR
    // folds it into the poly header; GLdc no-ops (it keys texenv off shader ids itself).
    void (*set_tex_env)(uint32_t mode);
    void (*set_viewport)(int x, int y, int width, int height);
    void (*set_scissor)(int x, int y, int width, int height);
    void (*set_use_alpha)(bool use_alpha);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    // 2D screen-space quad (4 verts). GLdc draws GL_QUADS; the raw-PVR backend submits a native
    // 4-vertex triangle strip. The front-end builds the verts in the order each backend wants.
    void (*draw_triangles_2d)(void *buf_vbo, size_t buf_vbo_len, size_t buf_vbo_num_tris);
    void (*init)(void);
    void (*on_resize)(void);
    void (*start_frame)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
};

#endif
