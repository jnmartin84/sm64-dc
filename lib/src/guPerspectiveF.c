#include "libultra_internal.h"
#include <math.h>
#include <stdint.h>
#include "sh4zam_port.h"

void guPerspectiveF(float mf[4][4], float fovy, float aspect, float near, float far, float scale) {
    float yscale;
    int row;
    int col;
    f32 recip_aspect = shz_invf(aspect);
    f32 recip_nsubf = shz_invf(near - far);

    guMtxIdentF(mf);
    // pi / 180
    fovy *= 0.01745329f;
    f32 recipsinf = shz_invf(sinf(fovy * 0.5f));
    yscale = cosf(fovy * 0.5f) * recipsinf;
    mf[0][0] = yscale * recip_aspect;
    mf[1][1] = yscale;
    mf[2][2] = (near + far) * recip_nsubf;
    mf[2][3] = -1.0f;
    mf[3][2] = 2.0f * near * far * recip_nsubf;
    mf[3][3] = 0.0f;
    if (scale != 1.0f) {
        for (row = 0; row < 4; row++) {
            for (col = 0; col < 4; col++) {
                mf[row][col] *= scale;
            }
        }
    }
}

void guPerspective(Mtx* m, float fovy, float aspect, float near, float far, float scale) {
#ifndef GBI_FLOATS
    float mat[4][4];
    guPerspectiveF(mat, fovy, aspect, near, far, scale);
    guMtxF2L(mat, m);
#else
    guPerspectiveF(m->m, fovy, aspect, near, far, scale);
#endif
}