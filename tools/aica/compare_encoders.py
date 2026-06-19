#!/usr/bin/env python3
"""
Three-way encoder comparison on the built sound_data.{ctl,tbl}, parallelized
across samples (stdlib multiprocessing -- no numpy/scipy, so it stays build-clean).

Isolates each encoder improvement on identical input:
    baseline : yamaha_adpcm.encode            (greedy, formulaic)
    v1       : yamaha_adpcm_v2.encode(beam=1)  (analysis-by-synthesis, no beam)
    v2       : yamaha_adpcm_v2.encode(beam=B)  (full beam search)

Each sample is encoded by all three in a worker process, then we print aggregate
stats + a worst-N table ranked by baseline segSNR. Use --jobs to set the pool
size (default = all cores).

Usage:
    python3 compare_encoders.py <ctl> <tbl> [--worst N] [--beam B] [--jobs J]
"""

import argparse
import math
import os
from multiprocessing import Pool

import sm64_albank_parse as albank
import yamaha_adpcm as base
import yamaha_adpcm_v2 as v2mod
from adpcm_quality import _to_pcm_input, _snr_db, _seg_snr_db, _peak_err

_BEAM = 32


def _metrics(ref, adpcm, n, enc):
    test = enc.decode(adpcm, n)
    m = min(len(ref), len(test))
    r, t = ref[:m], test[:m]
    return _snr_db(r, t), _seg_snr_db(r, t), _peak_err(r, t)


def _work(item):
    key, s = item
    ref, decimated = _to_pcm_input(s)
    if not ref:
        return None
    out = {"key": key, "n": len(ref), "loop": s.has_loop, "decim": decimated}
    a, n = base.encode(ref)
    out["base"] = _metrics(ref, a, n, base)
    a, n = v2mod.encode(ref, beam=1)
    out["v1"] = _metrics(ref, a, n, v2mod)
    a, n = v2mod.encode(ref, beam=_BEAM)
    out["v2"] = _metrics(ref, a, n, v2mod)
    return out


def _agg(rows, kind, idx):
    vals = [r[kind][idx] for r in rows if math.isfinite(r[kind][idx])]
    return (sum(vals) / len(vals), min(vals)) if vals else (float("nan"), float("nan"))


def main():
    global _BEAM
    ap = argparse.ArgumentParser()
    ap.add_argument("ctl")
    ap.add_argument("tbl")
    ap.add_argument("--worst", type=int, default=15)
    ap.add_argument("--beam", type=int, default=32)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()
    _BEAM = args.beam

    _, samples = albank.parse_all(args.ctl, args.tbl, with_data=True)
    items = list(samples.items())
    with Pool(args.jobs) as p:
        rows = [r for r in p.map(_work, items) if r]

    print(f"samples={len(rows)}  jobs={args.jobs}  beam={args.beam}\n")
    print(f"  {'encoder':<14}{'mean SNR':>10}{'mean segSNR':>13}{'min SNR':>10}")
    for label, kind in (("baseline", "base"), ("v1 (a-by-s)", "v1"), (f"v2 (beam{args.beam})", "v2")):
        msnr, _ = _agg(rows, kind, 0)
        mseg, _ = _agg(rows, kind, 1)
        _, minsnr = _agg(rows, kind, 0)
        print(f"  {label:<14}{msnr:>10.2f}{mseg:>13.2f}{minsnr:>10.2f}")

    rows.sort(key=lambda r: r["base"][1])
    print(f"\nworst {args.worst} by baseline segSNR  (segSNR dB | peak err):")
    print(f"  {'key':>8} {'len':>6} L  {'base':>6} {'v1':>6} {'v2':>6}   {'base pk':>7} {'v2 pk':>7}")
    for r in rows[: args.worst]:
        print(f"  {r['key']:>8x} {r['n']:>6} {int(r['loop'])}  "
              f"{r['base'][1]:>6.2f} {r['v1'][1]:>6.2f} {r['v2'][1]:>6.2f}   "
              f"{r['base'][2]:>7} {r['v2'][2]:>7}")


if __name__ == "__main__":
    main()
