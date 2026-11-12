#!/usr/bin/env python3
"""Check a converted SM64 song against the console recording it came from.

    tools/aica/sm64_afx_verify.py build/afx/bob.afx [--tolerance 3]

Replays the AFX stream (following its LOOP) into the sounding state of every
channel: sample, pitch, pan and level words. Rebuilds the same state from the
recording (<afx>.json names the log and the time mapping), on the same clock,
for the whole log: a looping song's second pass is compared with the game's
own second pass. Channel numbers are ignored (the game's allocator may pick
others in another pass); the state is the multiset of sounding voices.

Reports how many ticks the two disagree, as mismatch intervals: an interval
no longer than --tolerance ticks is a timing difference (rounding, events
moved a tick later to fit the ARM7's budget); longer ones are listed.
This checks the stream and its timing, not the register formulas: both sides
use sm64_afx.words(), which reproduces KOS's driver.
"""
import argparse
import json
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sm64_afx as conv

FIELDS = 18
F_PITCH, F_DIRECT, F_MIX, F_ENV_AD = conv.FIELD_PITCH, conv.FIELD_DIRECT, conv.FIELD_MIX, conv.FIELD_ENV_AD


def job_samples(path):
    """The sample bytes of each zone in a .job file (sm64_afx.py writes it next to the AFX)."""
    job = Path(path).read_bytes()
    zone_count = struct.unpack_from("<I", job, 4)[0]
    at, samples = 32, []
    for _ in range(zone_count):
        size = struct.unpack_from("<6I", job, at)[5]
        samples.append(job[at + 24:at + 24 + size])
        at += 24 + (size + 3) // 4 * 4
    return samples


def setup_zones(path, data, h, zones):
    """The zone key of each AFX setup. AICAforge builds its own setup templates (several can share a
    zone, in its own order), so a setup is matched to its zone by sample: the setup's binding record
    gives its sample's place in the AFB, whose bytes are the zone's in the .job file."""
    afb = Path(path).with_suffix(".afb").read_bytes()
    payload = struct.unpack_from("<I", afb, 16)[0]
    by_bytes = {}
    for i, sample in enumerate(job_samples(Path(path).with_suffix(".job"))):
        by_bytes.setdefault(sample, []).append(zones[i])
    keys = [None] * h[9]
    for r in range(h[13]):
        pair, offset, size = struct.unpack_from("<3I", data, h[12] + 12 * r)
        owners = by_bytes.get(afb[payload + offset:payload + offset + size], [])
        if len(set(owners)) != 1:
            sys.exit(f"setup {pair // (FIELDS * 2)}: its sample matches {len(set(owners))} zones of the .job")
        keys[pair // (FIELDS * 2)] = owners[0]
    return keys


def afx_timeline(path, zones, total):
    """[(tick, frozenset state)] change points of the AFX stream, played for `total` ticks."""
    data = Path(path).read_bytes()
    h = struct.unpack_from("<20I", data)
    image = data[h[4]:h[4] + h[5]]
    s0, size, setup_count = h[6], h[7], h[9]   # a lane map (AFX_FLAG_LANES) sits between setups and stream
    setups = [list(struct.unpack_from(f"<{FIELDS}H", image, i * FIELDS * 2)) for i in range(setup_count)]
    zones = setup_zones(path, data, h, zones)
    stream = image[s0:s0 + size]
    state = {}            # channel -> [setup, fields]
    changes, tick, at, loops = [], 0, 0, 0

    def snap():
        return frozenset_multiset((zones[st], f[F_PITCH], f[F_DIRECT], f[F_MIX]) for st, f in state.values())

    last = None
    while tick <= total:
        op = stream[at]
        if op in (1, 2, 3):
            n = {1: 1, 2: 2, 3: 4}[op]
            wait = int.from_bytes(stream[at + 1:at + 1 + n], "little")
            cur = snap()
            if cur != last:
                changes.append((tick, cur))
                last = cur
            tick += wait
            at += 1 + n
        elif op in (0x10, 0x14):
            ch, setup = stream[at + 1], struct.unpack_from("<H", stream, at + 2)[0]
            if op == 0x14:
                mask, vals, size_ = (1 << F_PITCH) | (1 << F_MIX), struct.unpack_from("<2H", stream, at + 4), 8
            else:
                mask = struct.unpack_from("<I", stream, at + 4)[0]
                n = bin(mask).count("1")
                vals, size_ = struct.unpack_from(f"<{n}H", stream, at + 8), 8 + 2 * n
            f = list(setups[setup])
            it = iter(vals)
            for fld in range(FIELDS):
                if mask >> fld & 1:
                    f[fld] = next(it)
            state[ch] = [setup, f]
            at += size_
        elif op in (0x11, 0x15, 0x17, 0x18):
            ch = stream[at + 1]
            if op != 0x11:     # the compact forms: MIX, ENV_AD, or both
                mask = {0x15: 1 << F_MIX, 0x17: 1 << F_ENV_AD, 0x18: 1 << F_ENV_AD | 1 << F_MIX}[op]
                n = bin(mask).count("1")
                vals, size_ = struct.unpack_from(f"<{n}H", stream, at + 2), 2 + 2 * n
            else:
                mask = struct.unpack_from("<I", stream, at + 2)[0]
                n = bin(mask).count("1")
                vals, size_ = struct.unpack_from(f"<{n}H", stream, at + 6), 6 + 2 * n
            if ch in state:
                it = iter(vals)
                for fld in range(FIELDS):
                    if mask >> fld & 1:
                        state[ch][1][fld] = next(it)
            at += size_
        elif op == 0x12:
            state.pop(stream[at + 1], None)
            at += 2
        elif op == 0x16:
            at = struct.unpack_from("<I", stream, at + 1)[0]
            loops += 1
        elif op in (0, 0x13):
            break
        else:
            sys.exit(f"unknown opcode {op:#x} at {at}")
    cur = snap()
    if cur != last:
        changes.append((tick, cur))
    return changes, loops


def frozenset_multiset(items):
    """The audible state: a voice at total level 0xFF is silent and left out, and a pan of
    zero attenuation is centre whichever side its sign bit names."""
    counts = {}
    for key, pitch, direct, mix in items:
        if mix >> 8 == 0xFF:
            continue
        if direct & 0x0F == 0:
            direct &= ~0x10
        it = (key, pitch, direct, mix)
        counts[it] = counts.get(it, 0) + 1
    return frozenset(counts.items())


def log_timeline(meta):
    upf, waves, raw = conv.read_log(meta["log"])
    scale = (meta["afx_rate"][0] / meta["afx_rate"][1]) / meta["sub_rate"]
    state, changes, last, tick = {}, [], None, 0
    records = [(round((r[0] - meta["first_sub"]) * scale),) + r[1:] for r in raw]
    i = 0
    while i < len(records):
        tick = records[i][0]
        while i < len(records) and records[i][0] == tick:
            t, cmd, ch, vol, pan, freq, key = records[i][:7]
            if cmd == conv.START:
                w = conv.words(freq, vol, pan)
                state[ch] = (conv.zone_key(key), w[F_PITCH], w[F_DIRECT], w[F_MIX])
            elif cmd == conv.UPDATE and ch in state:
                w = conv.words(freq, vol, pan)
                state[ch] = (state[ch][0], w[F_PITCH], w[F_DIRECT], w[F_MIX])
            elif cmd == conv.STOP:
                state.pop(ch, None)
            i += 1
        cur = frozenset_multiset(state.values())
        if cur != last:
            changes.append((tick, cur))
            last = cur
    end = max(r[0] for r in records)
    return changes, end


def state_at(changes, t, cache):
    """State at tick t from a sorted change list (cache holds the last index)."""
    k = cache[0]
    while k + 1 < len(changes) and changes[k + 1][0] <= t:
        k += 1
    cache[0] = k
    return changes[k][1] if changes and changes[0][0] <= t else frozenset()


def signed_pan(direct):
    return -(direct & 0x0F) if direct & 0x10 else direct & 0x0F


def close(a, b, pan_tol, level_tol):
    """Two sounding states agree: each voice pairs with one on the other side of the same sample,
    pitch and DIRECT bits but pan, within pan_tol pan steps and level_tol TL steps (a flow made with
    AICAforge's lossy simplification, AFX_SIMPLIFY)."""
    if a == b:
        return True
    if not (pan_tol or level_tol):
        return False
    xs = [v for v, n in a for _ in range(n)]
    ys = [v for v, n in b for _ in range(n)]
    if len(xs) != len(ys):
        return False
    for key, pitch, direct, mix in xs:
        for i, (k2, p2, d2, m2) in enumerate(ys):
            if (k2 == key and p2 == pitch and (d2 & ~0x1F) == (direct & ~0x1F) and (m2 & 0xFF) == (mix & 0xFF)
                    and abs(signed_pan(d2) - signed_pan(direct)) <= pan_tol and abs((m2 >> 8) - (mix >> 8)) <= level_tol):
                del ys[i]
                break
        else:
            return False
    return True


TOL = (0, 0)


def disagreements(x, y, lo, hi):
    """Intervals in [lo, hi) where the change lists x and y give different states."""
    points = sorted({t for t, _ in x if lo <= t < hi} | {t for t, _ in y if lo <= t < hi} | {lo})
    cx, cy = [0], [0]
    out, start = [], None
    for t in points:
        same = close(state_at(x, t, cx), state_at(y, t, cy), *TOL)
        if not same and start is None:
            start = t
        if same and start is not None:
            out.append((start, t))
            start = None
    if start is not None:
        out.append((start, hi))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("afx")
    ap.add_argument("--tolerance", type=int, default=3)
    ap.add_argument("--pan-tol", type=int, help="pan steps a value may differ (default: from AFX_SIMPLIFY, else 0)")
    ap.add_argument("--level-tol", type=int, help="TL steps a value may differ (default: from AFX_SIMPLIFY, else 0)")
    args = ap.parse_args()
    global TOL
    env = [int(v) for v in os.environ.get("AFX_SIMPLIFY", "0,0").split(",")]
    TOL = (args.pan_tol if args.pan_tol is not None else env[0],
           args.level_tol if args.level_tol is not None else env[1])
    meta = json.loads(Path(args.afx).with_suffix(".json").read_text())
    log, log_end = log_timeline(meta)
    afx, loops = afx_timeline(args.afx, meta["zones"], log_end)
    mismatches = disagreements(log, afx, 0, log_end)
    loop_tick, end = meta.get("loop_tick"), meta["end"]
    long_ = [(a, b) for a, b in mismatches if b - a > args.tolerance]
    print(f"compared {log_end} ticks ({log_end * meta['afx_rate'][1] / meta['afx_rate'][0]:.1f} s) of the recording; "
          f"the AFX looped {loops} time(s)")
    for name, lo, hi in (("pass 1", 0, end), ("after the LOOP", end, log_end)):
        part = [(max(a, lo), min(b, hi)) for a, b in mismatches if b > lo and a < hi]
        if hi <= lo:
            continue
        total = sum(b - a for a, b in part)
        print(f"{name}: disagreement {total} ticks ({100 * total / (hi - lo):.2f}%) in {len(part)} intervals, "
              f"{sum(1 for a, b in part if b - a > args.tolerance)} longer than {args.tolerance} ticks, "
              f"longest {max((b - a for a, b in part), default=0)}")
    if loop_tick is not None and end < log_end:
        # Baseline: the game's own next pass against its first. After the LOOP the AFX plays
        # the first pass's material, so it can match the game's later pass no better than this.
        shift = end - loop_tick
        shifted = [(t + shift, st) for t, st in log]
        base = disagreements(log, shifted, end, log_end)
        afx_part = [(max(a, end), b) for a, b in mismatches if b > end]
        tb, ta = sum(b - a for a, b in base), sum(b - a for a, b in afx_part)
        print(f"baseline, the game's next pass against its first over the same ticks: {tb} ticks; "
              f"the AFX after its LOOP: {ta} ticks ({ta - tb:+d})")
        extra = disagreements(shifted, afx, end, log_end)
        te = sum(b - a for a, b in extra)
        print(f"the AFX after its LOOP against the game's first pass, shifted by one pass: {te} ticks in "
              f"{len(extra)} intervals, longest {max((b - a for a, b in extra), default=0)}")
        for a, b in extra[:10]:
            la, lb = state_at(shifted, a, [0]), state_at(afx, a, [0])
            print(f"  {a}..{b}: first pass only {sorted(dict(la).keys() - dict(lb).keys())[:3]} ; "
                  f"afx only {sorted(dict(lb).keys() - dict(la).keys())[:3]}")
        long_ = [(a, b) for a, b in mismatches if b - a > args.tolerance and a < end]
    for a, b in long_[:20]:
        where = "pass 1" if a < end else "pass 2 (after LOOP)"
        la, lb = state_at(log, a, [0]), state_at(afx, a, [0])
        only_log = sorted(dict(la).keys() - dict(lb).keys())[:3]
        only_afx = sorted(dict(lb).keys() - dict(la).keys())[:3]
        print(f"  {a}..{b} ({b - a} ticks, {where}): game only {only_log} ; afx only {only_afx}")
    sys.exit(1 if long_ else 0)


if __name__ == "__main__":
    main()
