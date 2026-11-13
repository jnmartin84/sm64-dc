#include "libultra_internal.h"
#include "sh4zam_port.h"
void guNormalize(f32 *x, f32 *y, f32 *z) {

shz_vec3_t norm = shz_vec3_normalize((shz_vec3_t) { .x = *x, .y = *y, .z = *z });
*x = norm.x;
*y = norm.y;
*z = norm.z;
//    f32 tmp = 1.0f / sqrtf(*x * *x + *y * *y + *z * *z);
  //  *x = *x * tmp;
    //*y = *y * tmp;
    //*z = *z * tmp;
}
