#include "libultra_internal.h"

#include <math.h>
#include <stdint.h>

#include "sh4zam_port.h"

void guOrthoF(float m[4][4], float left, float right, float bottom, float top, float near, float far, float scale) {
    int row;
    int col;
    guMtxIdentF(m);
    f32 recip_rsubl = shz_invf((right - left));
    f32 recip_tsubb = shz_invf((top - bottom));
    f32 recip_fsubn = shz_invf((far - near));
    m[0][0] = 2.0f * recip_rsubl;
    m[1][1] = 2.0f * recip_tsubb;
    m[2][2] = -2.0f * recip_fsubn;
    m[3][0] = -(right + left) * recip_rsubl;
    m[3][1] = -(top + bottom) * recip_tsubb;
    m[3][2] = -(far + near) * recip_fsubn;
    m[3][3] = 1.0f;

    if (scale != 1.0f) {
        for (row = 0; row < 4; row++) {
            for (col = 0; col < 4; col++) {
                m[row][col] *= scale;
            }
        }
    }
}

void guOrtho(Mtx* m, float left, float right, float bottom, float top, float near, float far, float scale) {
#ifndef GBI_FLOATS
    float sp28[4][4];
    guOrthoF(sp28, left, right, bottom, top, near, far, scale);
    guMtxF2L(sp28, m);
#else
    guOrthoF(m->m, left, right, bottom, top, near, far, scale);
#endif
}