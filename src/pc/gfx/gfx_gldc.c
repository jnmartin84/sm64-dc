#include <stdint.h>
#include <assert.h>
#include <unistd.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>
#include <GL/glkos.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"
#include "gl_fast_vert.h"

#undef bool
#define bool uint8_t
#define true 1
#define false 0

static int fog_changed = 0;

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used;
    int num_inputs;
};

struct SamplerState {
    GLenum min_filter;
    GLenum mag_filter;
    GLenum wrap_s;
    GLenum wrap_t;
    GLuint tex;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;

static struct SamplerState tmu_state;

static const dc_fast_t *cur_buf = NULL;
static bool gl_blend = false;
static bool gl_depth = false;
static bool gl_npot = false;

extern int16_t fog_mul;
extern int16_t fog_ofs;
extern float gProjectNear;
extern float gProjectFar;

static void resample_16bit(const unsigned short *in, int inwidth, int inheight, unsigned short *out, int outwidth, int outheight) {
    int i, j;
    const unsigned short *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static inline uint32_t next_pot(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static inline uint32_t is_pot(const uint32_t v) {
    return (v & (v - 1)) == 0;
}

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return true;
}

float gl_fog_start;
float gl_fog_end;

void gfx_opengl_change_fog(void) {
    if (!fog_changed) {
        fog_changed = 1;

        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, (GLfloat)gl_fog_start * 0.675f);
        glFogf(GL_FOG_END, (GLfloat)gl_fog_end * 0.75f);
    }
}

static void gfx_opengl_apply_shader(struct ShaderProgram *prg) {
    // vertices are always there
    glVertexPointer(3, GL_FLOAT, sizeof(dc_fast_t), &cur_buf[0].vert);
    glTexCoordPointer(2, GL_FLOAT, sizeof(dc_fast_t), &cur_buf[0].texture);
    glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, sizeof(dc_fast_t), &cur_buf[0].color);
    // have texture
    if (prg->texture_used) {
        glEnable(GL_TEXTURE_2D);
    } else {
        glDisable(GL_TEXTURE_2D);
    }

    if (prg->shader_id & SHADER_OPT_FOG) {
        glEnable(GL_FOG);
        gfx_opengl_change_fog();
    } else {
        glDisable(GL_FOG);
    }

    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        // (horrible) alpha discard
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 1.0f / 3.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg)) {
        cur_shader->enabled = false;
        cur_shader = NULL;
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    if (cur_shader)
        cur_shader->enabled = false;
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used = ccf.used_texture;

    if (ccf.used_texture && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_texture) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    }

    prg->enabled = false;

    gfx_opengl_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint32_t shader_id) {
    size_t i;

    for (i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static bool gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs) {
    *num_inputs = prg->num_inputs;
    return prg->texture_used;
}

GLuint newest_texture = 0;

static void gfx_clear_all_textures(void) {
    GLuint index = 0;
    if (newest_texture != 0) {
        for (index = 2; index <= newest_texture; index++)
            glDeleteTextures(1, &index);

        tmu_state.tex = 0;
    }
    newest_texture = 0;
}

void gfx_clear_texidx(GLuint texidx) {
    GLuint index = texidx;
    glDeleteTextures(0, &index);
    if (tmu_state.tex == texidx)
        tmu_state.tex = 0;
}

static uint32_t gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    newest_texture = ret;
    return (uint32_t) ret;
}

static void gfx_opengl_select_texture(uint32_t texture_id) {
    tmu_state.tex = texture_id;
    glBindTexture(GL_TEXTURE_2D, texture_id);
}

/* Used for rescaling textures ROUGHLY into pow2 dims */
static uint16_t __attribute__((aligned(32))) scaled[64 * 64]; /* 8kb */

extern void reset_texcache(void);

void nuke_everything(void) {
    gfx_clear_all_textures();
    reset_texcache();
}

static void gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    unsigned int intFormat;
    if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
        intFormat = GL_ARGB1555_TWID_KOS;
    } else {
        intFormat = GL_ARGB4444_TWID_KOS;
    }

    if (!gl_npot) {
        // we don't support non power of two textures, scale to next power of two if necessary
        if ((!is_pot(width) || !is_pot(height)) || (width < 8) || (height < 8)) {
            int pwidth = next_pot(width);
            int pheight = next_pot(height);
            /*@Note: Might not need texture max sizes */
            if(pwidth > 128){
                pwidth = 128;
            }
            if(pheight > 128){
                pheight = 128;
            }

            /* Need texture min sizes */
            if(pwidth < 8){
                pwidth = 8;
            }
            if(pheight < 8){
                pheight = 8;
            }

            resample_16bit((const uint16_t *)rgba32_buf, width, height, (uint16_t*)scaled, pwidth, pheight);

            rgba32_buf = (uint8_t*)scaled;
            width = pwidth;
            height = pheight;
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, intFormat, width, height, 0, GL_BGRA, type, rgba32_buf);
}

static inline GLenum gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_MIRROR)
        return GL_MIRRORED_REPEAT;

    if (val & G_TX_CLAMP)
        return GL_CLAMP;

    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static inline void gfx_opengl_apply_tmu_state(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tmu_state.min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tmu_state.mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, tmu_state.wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tmu_state.wrap_t);
}

static void gfx_opengl_set_sampler_parameters(bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLenum filter = linear_filter ? GL_LINEAR : GL_NEAREST;

    const GLenum wrap_s = gfx_cm_to_opengl(cms);
    const GLenum wrap_t = gfx_cm_to_opengl(cmt);

    tmu_state.min_filter = filter;
    tmu_state.mag_filter = filter;
    tmu_state.wrap_s = wrap_s;
    tmu_state.wrap_t = wrap_t;

    // set state for the first texture right away
    gfx_opengl_apply_tmu_state();
}

static void gfx_opengl_set_depth_test(bool depth_test) {
    if (depth_test)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    gl_depth = z_upd;
    glDepthMask(z_upd);
}

static bool is_zmode_decal = false;

// Polyoffset currently doesn't work so gotta workaround it. 
static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    is_zmode_decal = zmode_decal;
    if (zmode_decal) {
        glDepthFunc(GL_LEQUAL);  
    } else {
        glDepthFunc(GL_LESS);  
    }
}


static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    gl_blend = use_alpha;
    if (use_alpha)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
}

// 0x01a00200
static void over_skybox_setup_pre(void) {
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE); // don’t write depth (so it won’t block later geometry)
}

static void over_skybox_setup_post(void) {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

extern int water_bomb;
extern int doing_skybox;
extern u16 gCutsceneMsgFade;
extern int in_trilerp;
extern int doing_peach;
extern int doing_bowser;
extern uint8_t trilerp_a;
extern int in_peach_scene;
extern int font_draw;
int doing_letter = 0;
extern int drawing_hand;
extern int do_radar_mark;
extern int cotmc_shadow;
extern int water_ring;
extern int env_a;
extern int ddd_ripple;
extern int cotmc_water;
extern int transition_verts;
void gfx_opengl_2d_projection(void);
void gfx_opengl_reset_projection(void);

static void gfx_opengl_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    cur_buf = (void*)buf_vbo;

    gfx_opengl_apply_shader(cur_shader);

    if (cur_shader->texture_used) {
        glEnable(GL_TEXTURE_2D);
    } else {
        glDisable(GL_TEXTURE_2D);
    }

    if (in_peach_scene) {
        if (cur_shader->shader_id == 0x01045045 || cur_shader->shader_id == 0x01045a00)
            doing_letter = 1;
        else
            doing_letter = 0;
    } else {
        doing_letter = 0;
    }
    
    if (cur_shader->shader_id == 0x0000038D) {
        // Face fix. 
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        glEnable(GL_BLEND);
    }
    
    /* Goddard specular */
    if(cur_shader->shader_id == 0x00000551){
        // draw goddard stuff twice
        // first, shaded polys only, no textures
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);

        glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);

        // after submitting, set all of the vertex colors to 
        // used to be solid white
        // now more of a gray
        // reduce intensity of the highlight
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
	    for(unsigned int i=0;i<3*buf_vbo_num_tris;i++) {
		    fast_vbo[i].color.packed = 0xff888888; //ffffffff;
	    }
        // enable texture/blend and set dest blend to ONE
        // it will draw again, applying the specular highlights
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    }

    if (cur_shader->shader_id == 0x1200045) { // skybox
        if (doing_skybox) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_LEQUAL);
            glDisable(GL_BLEND);
            glDisable(GL_FOG);
        }
    }

    if (cotmc_shadow) {
        // Adjust depth values slightly for zmode_decal objects
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        glDepthMask(GL_TRUE);
        
        // Push the geometry slightly away from the camera
        glPushMatrix();
        glTranslatef(0.0f, -2.1f, -0.9f); // magic values need fine tuning. 
    }

    if (is_zmode_decal) {
        // Adjust depth values slightly for zmode_decal objects
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        
        // Push the geometry slightly towards the camera
        glPushMatrix();
        glTranslatef(0.0f, 2.1f, 0.9f); // magic values need fine tuning. 
    }

    if (transition_verts || drawing_hand || water_bomb)
        over_skybox_setup_pre();

    if (drawing_hand) {
        glDepthFunc(GL_ALWAYS);
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
        for (size_t i=0;i<3*buf_vbo_num_tris;i++)
                fast_vbo[i].vert.z -= 10.0f;
    }

    if (in_trilerp) {
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
        glEnable(GL_BLEND);
        if (doing_peach) {
            if (!trilerp_a) return;
            for (size_t i=0;i<3*buf_vbo_num_tris;i++) {
                fast_vbo[i].color.array.a = trilerp_a;
                fast_vbo[i].vert.y -= 1.0f;
                fast_vbo[i].vert.z -= 1.0f;
            }
        } else if (doing_bowser) {
            if (trilerp_a == 255) return;
            for (size_t i=0;i<3*buf_vbo_num_tris;i++) {
                fast_vbo[i].color.array.a = 255 - trilerp_a;
            }
        }
    }

    if (doing_letter) {
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
        for(unsigned int i=0;i<3*buf_vbo_num_tris;i++)
            fast_vbo[i].color.array.a = gCutsceneMsgFade;
    }

    if (font_draw) {
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
        for (size_t i=0;i<3*buf_vbo_num_tris;i++)
                fast_vbo[i].vert.z += 5.0f;
    }

    if(do_radar_mark)
        gfx_opengl_2d_projection();

    if (water_ring) {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
        for(unsigned int i=0;i<3*buf_vbo_num_tris;i++)
            fast_vbo[i].color.array.a = env_a;
    }

    if (cotmc_water || ddd_ripple) {
        glEnable(GL_BLEND);
        dc_fast_t *fast_vbo = (dc_fast_t*)buf_vbo;
        for(unsigned int i=0;i<3*buf_vbo_num_tris;i++)
            fast_vbo[i].color.array.a = 0xB4;
    }

    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);

    if (water_ring)
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    if(do_radar_mark)
        gfx_opengl_reset_projection();

    if (transition_verts || drawing_hand || water_bomb)
        over_skybox_setup_post();
        
    if (cotmc_shadow || is_zmode_decal) {
        glPopMatrix();
        glDepthFunc(GL_LESS); // Reset depth function
    }
    
    // pretty sure this is needed)
    if (cur_shader->shader_id == 0x0000038D) {
        glDisable(GL_BLEND);
//        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
    
    if (cur_shader->shader_id == 0x1200045) { // skybox
        if (doing_skybox) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glEnable(GL_BLEND);
            glEnable(GL_FOG);
        }
    }

    // restore default blend mode after goddard draw
    if(cur_shader->shader_id == 0x551) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_BLEND);
    }
}

extern void gfx_opengl_2d_projection(void);
extern void gfx_opengl_reset_projection(void);
void gfx_opengl_draw_triangles_2d(void *buf_vbo, UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    cur_buf = (void*)buf_vbo;

    gfx_opengl_apply_shader(cur_shader);
    gfx_opengl_2d_projection();
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    if (buf_vbo_num_tris) {
        if (cur_shader->texture_used) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tmu_state.tex);
        }
    }

    glDrawArrays(GL_QUADS, 0, 4);

    gfx_opengl_reset_projection();
}

static inline bool gl_check_ext(const char *name) {
    static const char *extstr = NULL;

    if (extstr == NULL)
        extstr = (const char *)glGetString(GL_EXTENSIONS);

    if (!strstr(extstr, name)) {
        printf("GL extension not supported: %s\n", name);
        return false;
    }

    printf("GL extension detected: %s\n", name);
    return true;
}

static inline bool gl_get_version(int *major, int *minor, bool *is_es) {
    const char *vstr = (const char *)glGetString(GL_VERSION);
    if (!vstr || !vstr[0]) return false;

    if (!strncmp(vstr, "OpenGL ES ", 10)) {
        vstr += 10;
        *is_es = true;
    } else if (!strncmp(vstr, "OpenGL ES-CM ", 13)) {
        vstr += 13;
        *is_es = true;
    }

    return (sscanf(vstr, "%d.%d", major, minor) == 2);
}

#define sys_fatal printf

extern void getRamStatus(void);

extern int clear_color;

#include <stdint.h>
float clr, clg, clb;
static inline void rgba5551_to_rgbf(uint16_t c,
                                   float *r,
                                   float *g,
                                   float *b)
{
    // Extract 5-bit channels
    uint32_t ri = (c >> 11) & 0x1F;
    uint32_t gi = (c >> 6)  & 0x1F;
    uint32_t bi = (c >> 1)  & 0x1F;

    // Convert to [0,1]
    const float inv31 = 1.0f / 31.0f;
    *r = ri * inv31;
    *g = gi * inv31;
    *b = bi * inv31;
}

#include <kos.h>
static void gfx_opengl_init(void) {
    newest_texture = 0;

    clear_color = 0;

    GLdcConfig config;
    glKosInitConfig(&config);
    config.autosort_enabled = GL_TRUE;
    config.fsaa_enabled = GL_FALSE;
    config.initial_op_capacity = 2048+1024;
    config.initial_pt_capacity = 512+512;
    config.initial_tr_capacity = 2048+1024;
    config.initial_immediate_capacity = 0;

    if (vid_check_cable() != CT_VGA) {
        vid_set_mode(DM_640x480_NTSC_IL, PM_RGB565);
    } else {
        vid_set_mode(DM_640x480_VGA, PM_RGB565);
    }

    glKosInitEx(&config);

    if (vid_check_cable() != CT_VGA) {
        vid_set_mode(DM_640x480_NTSC_IL, PM_RGB565);
    } else {
        vid_set_mode(DM_640x480_VGA, PM_RGB565);
    }

    getRamStatus();
    fflush(stdout);

    // check GL version
    int vmajor, vminor;
    bool is_es = false;
    gl_get_version(&vmajor, &vminor, &is_es);
    if ((vmajor < 2 && vminor < 1) || is_es)
        sys_fatal("OpenGL 1.1+ is required.\nReported version: %s%d.%d\n", is_es ? "ES" : "", vmajor, vminor);

    printf("GL_VERSION = %s\n", glGetString(GL_VERSION));
    printf("GL_EXTENSIONS =\n%s\n", glGetString(GL_EXTENSIONS));

    // these also never change
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glViewport(0, 0, 640, 480);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    fog_changed = 0;
    rgba5551_to_rgbf(clear_color, &clr, &clg, &clb);

    glClearColor(clr, clg, clb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void gfx_opengl_end_frame(void) {
}

static void gfx_opengl_finish_render(void) {
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render
};