// DC_NATIVE: display lists run as native commands (nat_ops.h), included by gfx_retro_dc.c after the
// native renderer, whose compiled lists a call draws when it can (nat_try).
//
// nat_exec runs a list. A command not yet native is translated where it stands (nat_xlate) and then
// run: each command does what gfx_run_dl did for its GBI form, in the same order, so the picture is
// the interpreter's.

// The GBI command at cmd as its native command, in place. Only its first word changes, but for a
// G_TRI2's second triangle; a rectangle's later words stay as they are.
static void __attribute__((noinline)) nat_xlate(Gfx *cmd) {
    const uint32_t w0 = cmd->words.w0;
    const uintptr_t w1 = cmd->words.w1;
    uint32_t op, ops = w0 & 0xFFFFFF;
    if (w0 == 0x424C4E44) {   // the trilerp marker (gfx_run_dl)
        cmd->words.w0 = (uint32_t) N_TRILERP << 24;
        return;
    }
    switch (w0 >> 24) {
        case G_VTX:
            op = N_VTX;
            ops = ((w0 >> 12) & 0xFF) | ((((w0 >> 1) & 0x7F) - ((w0 >> 12) & 0xFF)) & 0xFF) << 8;
            break;
        case (uint8_t) G_TRI1:
            op = N_TRI1;
            ops = ((w0 >> 17) & 0x7F) | ((w0 >> 9) & 0x7F) << 8 | ((w0 >> 1) & 0x7F) << 16;
            break;
        case (uint8_t) G_TRI2:
            op = N_TRI2;
            ops = ((w0 >> 17) & 0x7F) | ((w0 >> 9) & 0x7F) << 8 | ((w0 >> 1) & 0x7F) << 16;
            cmd->words.w1 = ((w1 >> 17) & 0x7F) | ((w1 >> 9) & 0x7F) << 8 | ((w1 >> 1) & 0x7F) << 16;
            break;
        case G_DL:
            op = (w0 >> 16) & 1 ? N_BRANCH : N_CALL;
            ops = 0;
            break;
        case (uint8_t) G_ENDDL:
            op = N_END;
            ops = 0;
            break;
        case (uint8_t) G_NOOP:
            op = w1 ? N_TAG : N_SYNC;
            break;
        case G_MTX:
            op = N_MTX;
            ops = (w0 & 0xFF) ^ G_MTX_PUSH;
            break;
        case (uint8_t) G_POPMTX:
            op = N_POPMTX;
            ops = w1 / 64 < 0xFFFFFF ? (uint32_t) (w1 / 64) : 0xFFFFFF;
            break;
        case G_MOVEMEM: op = N_MOVEMEM; break;
        case (uint8_t) G_MOVEWORD: op = N_MOVEWORD; break;
        case (uint8_t) G_TEXTURE: op = N_TEXTURE; break;
        case G_GEOMETRYMODE: op = N_GEOMETRYMODE; break;
        case (uint8_t) G_SETOTHERMODE_L: op = N_OTHERMODE_L; break;
        case (uint8_t) G_SETOTHERMODE_H: op = N_OTHERMODE_H; break;
        case G_SETTIMG: op = N_SETTIMG; break;
        case G_LOADBLOCK: op = N_LOADBLOCK; break;
        case G_LOADTILE: op = N_LOADTILE; break;
        case G_SETTILE: op = N_SETTILE; break;
        case G_SETTILESIZE: op = N_SETTILESIZE; break;
        case G_SETENVCOLOR: op = N_SETENVCOLOR; break;
        case G_SETPRIMCOLOR: op = N_SETPRIMCOLOR; break;
        case G_SETFOGCOLOR: op = N_SETFOGCOLOR; break;
        case G_SETFILLCOLOR: op = N_SETFILLCOLOR; break;
        case G_SETCOMBINE: op = N_SETCOMBINE; break;
        case G_TEXRECT: op = N_TEXRECT; break;
        case G_TEXRECTFLIP: op = N_TEXRECTFLIP; break;
        case G_FILLRECT: op = N_FILLRECT; break;
        case G_SETSCISSOR: op = N_SETSCISSOR; break;
        case G_SETZIMG: op = N_SETZIMG; break;
        case G_SETCIMG: op = N_SETCIMG; break;
        default: op = N_SYNC; break;   // gfx_run_dl drew nothing with it either
    }
    cmd->words.w0 = op << 24 | ops;
}

// One command other than a vertex load, a triangle, a call, a branch or an end, as gfx_run_dl ran
// its GBI form, its new triangle state first. Returns the words it takes.
static int nat_cmd(const Gfx *cmd) {
    const uint32_t w0 = cmd->words.w0;
    const uintptr_t w1 = cmd->words.w1;   // a pointer, 64 bits on the host
    tri_state_bump();
    switch (w0 >> 24) {
        case N_TAG:
            gfx_dc_tag((uint32_t) w1);
            break;
        case N_TRILERP:
            if (w1 == 0xAAAAAAAA) {
                in_trilerp = 1;
                doing_peach = 0;
                doing_bowser = 0;
            } else if (w1 == 0xBBBBBBBB) {
                doing_peach = 1;
                doing_bowser = 0;
            } else if (w1 == 0xCCCCCCCC) {
                doing_bowser = 1;
                doing_peach = 0;
            } else if (w1 == 0xDDDDDDDD) {
                in_trilerp = 0;
                doing_bowser = 0;
                doing_peach = 0;
            }
            break;
        case N_MTX:
            gfx_sp_matrix(w0 & 0xFF, (const int32_t *) seg_addr(w1));
            break;
        case N_POPMTX:
            gfx_sp_pop_matrix(w0 & 0xFFFFFF);
            break;
        case N_NATIVE: {
            const NatCall *c = (const NatCall *) w1;
            c->fn(c->arg);
            break;
        }
        case N_MOVEMEM:
            gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(w1));
            break;
        case N_MOVEWORD:
            gfx_sp_moveword(C0(16, 8), w1);
            break;
        case N_TEXTURE:
            gfx_sp_texture(C1(16, 16), C1(0, 16));
            break;
        case N_GEOMETRYMODE:
            gfx_sp_geometry_mode(~C0(0, 24), w1);
            break;
        case N_OTHERMODE_L:
            gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, w1);
            break;
        case N_OTHERMODE_H:
            gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) w1 << 32);
            break;
        case N_SETTIMG:
            gfx_dp_set_texture_image(C0(19, 2), C0(0, 10), seg_addr(w1));
            break;
        case N_LOADBLOCK:
            gfx_dp_load_block(C1(12, 12));
            break;
        case N_LOADTILE:
            gfx_dp_load_tile(C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
            break;
        case N_SETTILE:
            gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C1(24, 3), C1(18, 2), C1(8, 2));
            break;
        case N_SETTILESIZE:
            gfx_dp_set_tile_size(C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
            break;
        case N_SETENVCOLOR:
            gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
            break;
        case N_SETPRIMCOLOR:
            gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
            break;
        case N_SETFOGCOLOR:
            gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
            break;
        case N_SETFILLCOLOR:
            gfx_dp_set_fill_color((uint32_t) w1);
            break;
        case N_SETCOMBINE:
            gfx_dp_set_combine_mode(color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)));
            rdp.combine_w0 = (uint32_t) G_SETCOMBINE << 24 | (w0 & 0xFFFFFF);   // the GBI words
            rdp.combine_w1 = (uint32_t) w1;
            break;
        case N_TEXRECT:
        case N_TEXRECTFLIP: {
            ISTAT(interp_stats_rect());
            const int32_t lrx = (int32_t) (C0(0, 24) << 8) >> 8, lry = (int32_t) (C1(0, 24) << 8) >> 8;
            const int32_t ulx = (int32_t) ((cmd[1].words.w0 & 0xFFFFFF) << 8) >> 8;
            const int32_t uly = (int32_t) ((cmd[1].words.w1 & 0xFFFFFF) << 8) >> 8;
            const uint32_t uls = cmd[2].words.w0 >> 16, ult = cmd[2].words.w0 & 0xFFFF;
            const uint32_t dsdx = cmd[2].words.w1 >> 16, dtdy = cmd[2].words.w1 & 0xFFFF;
            gfx_dp_texture_rectangle(ulx, uly, lrx, lry, uls, ult, dsdx, dtdy, (w0 >> 24) == N_TEXRECTFLIP);
            return 3;
        }
        case N_FILLRECT: {
            ISTAT(interp_stats_rect());
            const int32_t lrx = (int32_t) (C0(0, 24) << 8) >> 8, lry = (int32_t) (C1(0, 24) << 8) >> 8;
            const int32_t ulx = (int32_t) ((cmd[1].words.w0 & 0xFFFFFF) << 8) >> 8;
            const int32_t uly = (int32_t) ((cmd[1].words.w1 & 0xFFFFFF) << 8) >> 8;
            gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
            return 2;
        }
        case N_SETSCISSOR:
            gfx_dp_set_scissor(C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
            break;
        case N_SETZIMG:
            gfx_dp_set_z_image(seg_addr(w1));
            break;
        case N_SETCIMG:
            gfx_dp_set_color_image(seg_addr(w1));
            break;
        default:   // N_SYNC
            break;
    }
    return 1;
}

// One command other than a call, a branch or an end (the recording's, nat_record). Returns the
// words it takes.
static int nat_one(const Gfx *cmd) {
    const uint32_t w0 = cmd->words.w0;
    const uintptr_t w1 = cmd->words.w1;
    switch (w0 >> 24) {
        case N_VTX:
            gfx_sp_vertex(w0 & 0xFF, (w0 >> 8) & 0xFF, (const Vtx *) seg_addr(w1));
            return 1;
        case N_TRI1:
            gfx_sp_tri1(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF);
            return 1;
        case N_TRI2:
            gfx_sp_tri1(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF);
            gfx_sp_tri1(w1 & 0xFF, (w1 >> 8) & 0xFF, (w1 >> 16) & 0xFF);
            return 1;
        default:
            return nat_cmd(cmd);
    }
}

// Run the list at cmd: a call draws the compiled list when there is one, else runs into it.
static void SM_HOT __attribute__((noinline)) nat_exec(Gfx *cmd) {
    Gfx *stack[GFX_DL_STACK_MAX];
    int sp = 0;
    for (;;) {
        uint32_t w0 = cmd->words.w0;
        if (!NAT_IS_OP(w0 >> 24)) {
            nat_xlate(cmd);
            w0 = cmd->words.w0;
        }
        __builtin_prefetch((const char *) cmd + 32);
        ISTAT(interp_stats_cmd(w0 >> 24));
        switch (w0 >> 24) {
            case N_VTX:
                ISTAT(interp_stats_vtx(w0 & 0xFF));
                gfx_sp_vertex(w0 & 0xFF, (w0 >> 8) & 0xFF, (const Vtx *) seg_addr(cmd->words.w1));
                cmd++;
                continue;
            case N_TRI1:
                ISTAT(interp_stats_tri(1));
                gfx_sp_tri1(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF);
                cmd++;
                continue;
            case N_TRI2: {
                const uint32_t w1 = cmd->words.w1;
                ISTAT(interp_stats_tri(2));
                gfx_sp_tri1(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF);
                gfx_sp_tri1(w1 & 0xFF, (w1 >> 8) & 0xFF, (w1 >> 16) & 0xFF);
                cmd++;
                continue;
            }
            case N_CALL:
            case N_BRANCH: {
                Gfx *to = (Gfx *) seg_addr(cmd->words.w1);
                const int call = (w0 >> 24) == N_CALL;
                if (nat_try(to, cmd)) {
                    if (call) {
                        cmd++;
                    } else if (sp == 0) {
                        return;   // branched to: it ended this list
                    } else {
                        cmd = stack[--sp];
                    }
                    continue;
                }
                __builtin_prefetch(to);
                ISTAT(interp_stats_call(to, call));
                if (call) {
                    if (sp == GFX_DL_STACK_MAX) return;   // deeper than any list the game builds
                    stack[sp++] = cmd + 1;
                }
                cmd = to;
                continue;
            }
            case N_END:
                if (sp == 0) return;
                ISTAT(interp_stats_return());
                cmd = stack[--sp];
                __builtin_prefetch(cmd);
                continue;
            default:
                cmd += nat_cmd(cmd);
                continue;
        }
    }
}
