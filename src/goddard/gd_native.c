#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "gd_types.h"
#include "renderer.h"
#include "gd_native.h"

#include <stdlib.h>
#include <string.h>
#include "pc/gfx/native/nat_mesh.h"

#include <malloc.h>
#define gd_nat_memalign(n) memalign(32, n)

// A shape's display list drawn as native meshes (nat_mesh.h).
//
// create_shape_gddl builds the list once: groups of up to 12 vertices, each loaded and followed by
// the triangles of the faces that use them, and a material's list called where the material
// changes. A vertex is copied into every group whose faces use it, so the Mario head's face, 440
// vertices, loads 2019 copies a frame, each transformed and lit again. Every frame
// convert_gd_verts_to_Vn/_Vtx writes a vertex's position (and normal) into each of its copies
// through its links (gbiVerts).
//
// Here each run of groups between two state commands keeps one copy of each vertex it uses
// (NatMeshRun): its vertices load once a frame, and its triangles index them. The list keeps its
// state commands, material calls included, where they were, and has one G_NATIVE command for each
// run in place of its loads and triangles; a vertex's links point at its copies in the runs instead.
// The image is the display list's: the copies of a vertex are the same, and nothing between a run's
// triangles changes the state they draw in.

#define GD_NAT_MAX_CMDS 4096
#define GD_NAT_MAX_COPIES 8192
#define GD_NAT_MAX_RUNS 64
#define GD_NAT_MAX_TRIS 4096
#define GD_NAT_HASH 1024   // a power of two above NAT_MESH_VERTS

typedef struct {
    Vtx *copy;
    struct ObjVertex *owner;
} CopyOwner;

typedef struct {
    struct ObjVertex *owner;
    Vtx *vtx;
} ClassLink;

typedef struct {
    // The list's copies of each vertex, sorted by address, and whose they are.
    CopyOwner copies[GD_NAT_MAX_COPIES];
    int ncopies;
    // The new list: a command, or (w0 == G_NATIVE << 24) the index of a run in w1.
    Gfx out[GD_NAT_MAX_CMDS];
    int nout;
    // The run being built: its vertices, whose they are, and its triangles.
    int open;
    Vtx vtx[NAT_MESH_VERTS];
    struct ObjVertex *owner[NAT_MESH_VERTS];
    int nvtx;
    uint8_t tri[GD_NAT_MAX_TRIS][3];
    int ntri;
    struct ObjVertex *hkey[GD_NAT_HASH];
    uint16_t hval[GD_NAT_HASH];
    // What each slot holds: a vertex of run slot_run[i] (its index slot_vtx[i]).
    int slot_run[64];
    int slot_vtx[64];
    // The finished runs.
    NatMeshRun *runs[GD_NAT_MAX_RUNS];
    struct ObjVertex **run_owner[GD_NAT_MAX_RUNS];
    int nruns;
} Build;

static int cmp_copy(const void *a, const void *b) {
    const uintptr_t x = (uintptr_t) ((const CopyOwner *) a)->copy, y = (uintptr_t) ((const CopyOwner *) b)->copy;
    return x < y ? -1 : x > y;
}

static int cmp_link(const void *a, const void *b) {
    const uintptr_t x = (uintptr_t) ((const ClassLink *) a)->owner, y = (uintptr_t) ((const ClassLink *) b)->owner;
    return x < y ? -1 : x > y;
}

static struct ObjVertex *owner_of(const Build *b, const Vtx *v) {
    int lo = 0, hi = b->ncopies - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (b->copies[mid].copy == v) return b->copies[mid].owner;
        if ((uintptr_t) b->copies[mid].copy < (uintptr_t) v) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// Where vertex o's copy in the open run is in the hash: its index there, or an empty entry.
static uint32_t run_slot(const Build *b, const struct ObjVertex *o) {
    uint32_t h = (uint32_t) (((uintptr_t) o >> 3) * 0x9E3779B1u) & (GD_NAT_HASH - 1);
    while (b->hkey[h] != NULL && b->hkey[h] != o) h = (h + 1) & (GD_NAT_HASH - 1);
    return h;
}

// The open run's index for a vertex: its copy there, made from v if it has none yet.
static int run_vertex(Build *b, struct ObjVertex *o, const Vtx *v) {
    const uint32_t h = run_slot(b, o);
    if (b->hkey[h] == o) return b->hval[h];
    b->hkey[h] = o;
    b->hval[h] = (uint16_t) b->nvtx;
    b->vtx[b->nvtx] = *v;
    b->owner[b->nvtx] = o;
    return b->nvtx++;
}

// The open run, finished: its memory, and its command in the new list.
static int close_run(Build *b) {
    if (!b->open) return 1;
    b->open = 0;
    if (b->nruns == GD_NAT_MAX_RUNS || b->nout == GD_NAT_MAX_CMDS) return 0;
    NatMeshRun *r = gd_nat_memalign(sizeof(NatMeshRun));
    Vtx *vtx = gd_nat_memalign(b->nvtx * sizeof(Vtx));
    uint8_t (*tri)[3] = gd_nat_memalign(b->ntri * 3 + 1);
    struct ObjVertex **own = malloc(b->nvtx * sizeof(*own));
    if (r == NULL || vtx == NULL || tri == NULL || own == NULL) return 0;
    memcpy(vtx, b->vtx, b->nvtx * sizeof(Vtx));
    memcpy(tri, b->tri, b->ntri * 3);
    memcpy(own, b->owner, b->nvtx * sizeof(*own));
    r->vtx = vtx;
    r->tri = tri;
    r->nvtx = (uint16_t) b->nvtx;
    r->ntri = (uint16_t) b->ntri;
    gfx_native_mesh_run_init(r);
    b->runs[b->nruns] = r;
    b->run_owner[b->nruns] = own;
    b->out[b->nout].words.w0 = (uintptr_t) G_NATIVE << 24;
    b->out[b->nout].words.w1 = (uintptr_t) b->nruns;
    b->nout++;
    b->nruns++;
    return 1;
}

static void open_run(Build *b) {
    b->open = 1;
    b->nvtx = 0;
    b->ntri = 0;
    memset(b->hkey, 0, sizeof(b->hkey));
}

static int add_tri(Build *b, int s0, int s1, int s2) {
    const int s[3] = { s0, s1, s2 };
    if (!b->open || b->ntri == GD_NAT_MAX_TRIS) return 0;
    for (int i = 0; i < 3; i++) {
        // A slot this run loaded: one loaded before (another run's state) would light differently.
        if (s[i] >= 64 || b->slot_run[s[i]] != b->nruns) return 0;
        b->tri[b->ntri][i] = (uint8_t) b->slot_vtx[s[i]];
    }
    b->ntri++;
    return 1;
}

static int build(Build *b, struct ObjShape *shape, Gfx *dl, s32 ngfx) {
    b->ncopies = 0;
    for (struct Links *l = shape->vtxGroup->link1C; l != NULL; l = l->next) {
        struct ObjVertex *v = (struct ObjVertex *) l->obj;
        for (struct VtxLink *k = v->gbiVerts; k != NULL; k = k->prev) {
            if (b->ncopies == GD_NAT_MAX_COPIES) return 0;
            b->copies[b->ncopies].copy = k->data;
            b->copies[b->ncopies].owner = v;
            b->ncopies++;
        }
    }
    qsort(b->copies, b->ncopies, sizeof(CopyOwner), cmp_copy);
    for (int i = 0; i < 64; i++) b->slot_run[i] = -1;

    for (s32 i = 0; i < ngfx; i++) {
        const Gfx *g = &dl[i];
        const uint32_t w0 = (uint32_t) g->words.w0, op = w0 >> 24;
        if (op == G_VTX) {
            const int n = (w0 >> 12) & 0xFF, dest = ((w0 >> 1) & 0x7F) - n;
            const Vtx *src = (const Vtx *) g->words.w1;
            if (dest < 0 || dest + n > 32) return 0;
            if (!b->open) open_run(b);
            // A group the run has no room for starts the next one (in the same state).
            int fresh = 0;
            for (int k = 0; k < n; k++) {
                const struct ObjVertex *o = owner_of(b, &src[k]);
                if (o == NULL) return 0;
                fresh += b->hkey[run_slot(b, o)] != o;
            }
            if (b->nvtx + fresh > NAT_MESH_VERTS) {
                if (!close_run(b)) return 0;
                open_run(b);
            }
            for (int k = 0; k < n; k++) {
                struct ObjVertex *o = owner_of(b, &src[k]);
                b->slot_run[dest + k] = b->nruns;
                b->slot_vtx[dest + k] = run_vertex(b, o, &src[k]);
            }
        } else if (op == (uint8_t) G_TRI1) {
            if (!add_tri(b, (w0 >> 17) & 0x7F, (w0 >> 9) & 0x7F, (w0 >> 1) & 0x7F)) return 0;
        } else if (op == (uint8_t) G_TRI2) {
            const uint32_t w1 = (uint32_t) g->words.w1;
            if (!add_tri(b, (w0 >> 17) & 0x7F, (w0 >> 9) & 0x7F, (w0 >> 1) & 0x7F)) return 0;
            if (!add_tri(b, (w1 >> 17) & 0x7F, (w1 >> 9) & 0x7F, (w1 >> 1) & 0x7F)) return 0;
        } else if (op == (uint8_t) G_ENDDL) {
            if (!close_run(b) || b->nout == GD_NAT_MAX_CMDS) return 0;
            b->out[b->nout++] = *g;
            return b->nout <= ngfx;
        } else if (op == G_DL && ((w0 >> 16) & 1) != 0) {
            return 0;   // a branch: the list goes on elsewhere
        } else if (op == G_TEXRECT || op == G_TEXRECTFLIP || op == G_FILLRECT || op == G_NATIVE) {
            return 0;
        } else {
            // State (a material call, a render mode): it ends the run.
            if (!close_run(b) || b->nout == GD_NAT_MAX_CMDS) return 0;
            b->out[b->nout++] = *g;
        }
    }
    return 0;   // no end
}

// Every vertex's links point at its copies in the runs. A run's copy of a vertex came from one of
// its copies in the list (each loaded once), so a vertex has at least as many links as it needs.
static int relink(Build *b) {
    int nlinks = 0;
    for (int r = 0; r < b->nruns; r++) nlinks += b->runs[r]->nvtx;
    ClassLink *links = malloc(nlinks * sizeof(ClassLink) + 1);
    if (links == NULL) return 0;
    nlinks = 0;
    for (int r = 0; r < b->nruns; r++) {
        for (int c = 0; c < b->runs[r]->nvtx; c++) {
            links[nlinks].owner = b->run_owner[r][c];
            links[nlinks].vtx = &b->runs[r]->vtx[c];
            nlinks++;
        }
    }
    qsort(links, nlinks, sizeof(ClassLink), cmp_link);
    int ok = 1;
    for (int i = 0; i < nlinks && ok;) {
        struct ObjVertex *o = links[i].owner;
        int n = 0, have = 0;
        while (i + n < nlinks && links[i + n].owner == o) n++;
        for (struct VtxLink *k = o->gbiVerts; k != NULL; k = k->prev) have++;
        ok = have >= n;
        i += n;
    }
    for (int i = 0; i < nlinks && ok;) {
        struct ObjVertex *o = links[i].owner;
        struct VtxLink *k = o->gbiVerts, *last = NULL;
        for (; i < nlinks && links[i].owner == o; i++, k = k->prev) {
            k->data = links[i].vtx;
            last = k;
        }
        last->prev = NULL;
    }
    free(links);
    return ok;
}

s32 gd_native_shape(struct ObjShape *shape, Gfx *dl, s32 ngfx) {
    if (shape->vtxGroup == NULL) return 0;
    Build *b = calloc(1, sizeof(Build));
    if (b == NULL) return 0;
    int ok = build(b, shape, dl, ngfx) && relink(b);
    if (ok) {
        for (int i = 0; i < b->nout; i++) {
            dl[i] = b->out[i];
            if ((dl[i].words.w0 >> 24) == G_NATIVE) {
                dl[i].words.w1 = (uintptr_t) &b->runs[b->out[i].words.w1]->call;
            }
        }
    } else {
        for (int r = 0; r < b->nruns; r++) {
            free(b->runs[r]->vtx);
            free(b->runs[r]->tri);
            free(b->runs[r]);
        }
    }
    for (int r = 0; r < b->nruns; r++) free(b->run_owner[r]);
    free(b);
    return ok;
}
