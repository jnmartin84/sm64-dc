#ifndef GD_NATIVE_H
#define GD_NATIVE_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "gd_types.h"

// A shape's display list (create_shape_gddl), rewritten to draw its geometry as native meshes
// (gd_native.c). Returns 0 and leaves the list as it was if it cannot.
s32 gd_native_shape(struct ObjShape *shape, Gfx *dl, s32 ngfx);
Gfx *Unknown8019BE40(s32 num);   // renderer.c: a GdDisplayList's commands

#endif
