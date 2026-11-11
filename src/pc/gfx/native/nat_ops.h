#ifndef NAT_OPS_H
#define NAT_OPS_H
// The commands display lists run as (native/nat_exec.inc.c). A GBI command is
// translated into its native command the first time it runs, in place and in as many words: a list
// in the program's data is decoded once for good, a list built at run time once per logic step (the
// 60 fps second draw runs the native words). The opcodes are ones F3DEX2 leaves unused, so a list
// can hold both, as one the game rewrites in part does.
#include <stdint.h>

// w0 = opcode << 24 | operands, w1 as the GBI command's unless said.
enum {
    // Commands that keep the triangle state (gfx_run_dl began no new one at them):
    N_VTX = 0x10,     // n | dest << 8; w1 the vertices
    N_TRI1,           // gfx_sp_tri1's three arguments, a byte each
    N_TRI2,           // the first triangle; w1 the second, as the first
    N_CALL,           // w1 the list called
    N_BRANCH,         // w1 the list branched to
    N_END,
    // Every other command begins a new one:
    N_SYNC,           // a command with no other effect (a sync, an RDP command not drawn with)
    N_TAG,            // a Dreamcast tag (gfx_dc_tags.h) in w1
    N_TRILERP,        // the painting trilerp marker, which one in w1
    N_MTX,            // gfx_sp_matrix's parameters; w1 the matrix
    N_POPMTX,         // the count
    N_NATIVE,         // w1 a NatCall (nat_mesh.h)
    // These keep the GBI command's operands (its low 24 bits) as they were:
    N_MOVEMEM,
    N_MOVEWORD,
    N_TEXTURE,
    N_GEOMETRYMODE,
    N_OTHERMODE_L,
    N_OTHERMODE_H,
    N_SETTIMG,
    N_LOADBLOCK,
    N_LOADTILE,
    N_SETTILE,
    N_SETTILESIZE,
    N_SETENVCOLOR,
    N_SETPRIMCOLOR,
    N_SETFOGCOLOR,
    N_SETFILLCOLOR,
    N_SETCOMBINE,
    N_TEXRECT,        // and the two words after it, untouched
    N_TEXRECTFLIP,
    N_FILLRECT,       // and the word after it
    N_SETSCISSOR,
    N_SETZIMG,
    N_SETCIMG,
    N_LAST = N_SETCIMG
};
#define NAT_IS_OP(op) ((uint32_t) (op) - N_VTX <= (uint32_t) (N_LAST - N_VTX))

#endif
