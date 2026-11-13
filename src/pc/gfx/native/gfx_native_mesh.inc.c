// Native meshes (nat_mesh.h), included by gfx_retro_dc.c after the native renderer.
//
// A run's vertices load into a vertex buffer of their own (sVB, VtxBuf) as many at a time as the
// vertex loops take, and its triangles are drawn as gfx_sp_tri1 draws them (nat_tris) with indices
// into it. The loads, the triangle path and the bakes are the display list's: a vertex loaded once
// gives what each of its copies would have, as nothing changes the state within a run. Runs draw one at a time and keep
// nothing after, so one buffer serves them all.

static struct LoadedVertex __attribute__((aligned(32))) sMeshLV[NAT_MESH_VERTS + 4];
static dc_fast_t __attribute__((aligned(32))) sMeshBaked[NAT_MESH_VERTS + 4];
static float __attribute__((aligned(32))) sMeshProj[NAT_MESH_VERTS + 4][4];
static uint32_t __attribute__((aligned(8))) sMeshBakeEpoch[NAT_MESH_VERTS + 4];
static uint32_t sMeshProjGen[NAT_MESH_VERTS + 4];
static uint8_t __attribute__((aligned(32))) sMeshClip[NAT_MESH_VERTS + 4];
static uint8_t sMeshLit[NAT_MESH_VERTS + 4];

static void gfx_native_mesh_run_draw(const void *arg) {
    const NatMeshRun *r = arg;
    const VtxBuf slots = sVB;
    sVB.lv = sMeshLV;
    sVB.clip = sMeshClip;
    sVB.bake_epoch = sMeshBakeEpoch;
    sVB.baked = sMeshBaked;
    sVB.proj = sMeshProj;
    sVB.proj_gen = sMeshProjGen;
    sVB.lit = sMeshLit;
    for (uint32_t k = 0; k < r->nvtx; k += MAX_VERTICES) {
        const uint32_t n = r->nvtx - k < MAX_VERTICES ? r->nvtx - k : MAX_VERTICES;
        gfx_sp_vertex(n, k, &r->vtx[k]);
    }
    nat_tris(r->tri, r->ntri, 0);
    sVB = slots;
}

void gfx_native_mesh_run_init(NatMeshRun *run) {
    run->call.fn = gfx_native_mesh_run_draw;
    run->call.arg = run;
}
