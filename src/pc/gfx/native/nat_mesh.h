#ifndef NAT_MESH_H
#define NAT_MESH_H
// Geometry a game system hands the renderer directly instead of as display-list
// commands (gfx_native_mesh.inc.c). It is drawn with the state the display list has set where it
// stands, and its triangles come out as the commands would have drawn them.
#include <stdint.h>
#include <PR/gbi.h>
#include "nat_ops.h"

// A display-list command whose w1 is a NatCall: running the list calls fn(arg) (nat_ops.h).
#define G_NATIVE N_NATIVE
typedef struct {
    void (*fn)(const void *arg);
    const void *arg;
} NatCall;
#define gsSPNativeCall(call) { { (uintptr_t) G_NATIVE << 24, (uintptr_t) (call) } }

// A run of triangles over vertices that load once each, however many triangles use them: the
// vertices a run of loads and triangles drew, each once, with the triangles' corners as indices into
// them (in gfx_sp_tri1's order). Its producer keeps the vertices up to date; nothing in the state may
// change between its triangles. call is the command's NatCall (gfx_native_mesh_run_init).
#define NAT_MESH_VERTS 256
typedef struct {
    NatCall call;
    Vtx *vtx;
    uint8_t (*tri)[3];
    uint16_t nvtx, ntri;
} NatMeshRun;

void gfx_native_mesh_run_init(NatMeshRun *run);

#endif
