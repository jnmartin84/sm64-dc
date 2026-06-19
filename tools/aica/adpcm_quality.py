#!/usr/bin/env python3
"""
Measurement harness for AICA Yamaha-ADPCM encode quality (offline, stdlib-only).

For every sample in the built sound_data.{ctl,tbl}, isolates the loss introduced
by the Yamaha-ADPCM round-trip:

    ref  = VADPCM->PCM16            (the encoder INPUT -- ear-validated "good")
    test = decode(encode(ref))      (what actually plays on the AICA)

and reports SNR + segmental SNR + peak error per sample, ranked worst-first, plus
aggregate stats. Writes listenable WAVs (ref / test / difference) for the worst N
so we can A/B by ear. Pass an alternate encoder module with --encoder to compare a
new encoder against the current one on identical input.

Usage:
    python3 adpcm_quality.py <ctl> <tbl> [--out DIR] [--worst N] [--rate HZ]
                             [--encoder MODULE]
"""

import argparse
import importlib
import math
import os
import struct
import wave

import sm64_albank_parse as albank
import vadpcm
import yamaha_adpcm
import transcode

AICA_MAX = transcode.AICA_MAX


def _to_pcm_input(s):
    """Reproduce transcode's encoder input: VADPCM->PCM, 2x decimate if > cap."""
    pcm = vadpcm.decode(s.data, s.codec, s.order, s.npredictors, s.book)
    decimated = False
    if len(pcm) > AICA_MAX:
        pcm = transcode._decimate2(pcm)
        decimated = True
    return pcm, decimated


def _snr_db(ref, test):
    """Whole-signal SNR in dB (ref power / error power)."""
    sig = err = 0.0
    for a, b in zip(ref, test):
        sig += float(a) * a
        err += float(a - b) * (a - b)
    if err == 0.0:
        return float("inf")
    if sig == 0.0:
        return float("-inf")
    return 10.0 * math.log10(sig / err)


def _seg_snr_db(ref, test, win=256):
    """Segmental SNR: mean of per-window SNR (dB), clamped to [-10, 60] per the
    usual convention so silent/near-silent windows don't dominate. Correlates with
    perceived quality better than whole-signal SNR."""
    n = len(ref)
    if n == 0:
        return float("nan")
    total = 0.0
    count = 0
    for base in range(0, n, win):
        sig = err = 0.0
        for i in range(base, min(base + win, n)):
            a = float(ref[i]); b = float(test[i])
            sig += a * a
            err += (a - b) * (a - b)
        if sig <= 1.0:           # treat as silence, skip
            continue
        if err == 0.0:
            seg = 60.0
        else:
            seg = 10.0 * math.log10(sig / err)
            seg = -10.0 if seg < -10.0 else 60.0 if seg > 60.0 else seg
        total += seg
        count += 1
    return total / count if count else float("nan")


def _peak_err(ref, test):
    m = 0
    for a, b in zip(ref, test):
        d = a - b
        if d < 0:
            d = -d
        if d > m:
            m = d
    return m


def _write_wav(path, pcm, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        clamped = bytearray()
        for v in pcm:
            v = -32768 if v < -32768 else 32767 if v > 32767 else int(v)
            clamped += struct.pack("<h", v)
        w.writeframes(bytes(clamped))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ctl")
    ap.add_argument("tbl")
    ap.add_argument("--out", default="/tmp/adpcm_q")
    ap.add_argument("--worst", type=int, default=12)
    ap.add_argument("--rate", type=int, default=32000, help="WAV playback rate (artifacts audible regardless)")
    ap.add_argument("--encoder", default="yamaha_adpcm", help="encoder module with encode()/decode()")
    args = ap.parse_args()

    enc = importlib.import_module(args.encoder)
    os.makedirs(args.out, exist_ok=True)

    _, samples = albank.parse_all(args.ctl, args.tbl, with_data=True)
    rows = []
    for key, s in samples.items():
        ref, decimated = _to_pcm_input(s)
        if not ref:
            continue
        adpcm, n = enc.encode(ref)
        test = enc.decode(adpcm, n)
        # encode() may emit ceil(n/2)*2 nibbles; align lengths
        m = min(len(ref), len(test))
        ref_a, test_a = ref[:m], test[:m]
        rows.append({
            "key": key,
            "n": m,
            "loop": s.has_loop,
            "decim": decimated,
            "nbanks": len(s.banks),
            "snr": _snr_db(ref_a, test_a),
            "segsnr": _seg_snr_db(ref_a, test_a),
            "peak": _peak_err(ref_a, test_a),
            "ref": ref_a,
            "test": test_a,
        })

    rows.sort(key=lambda r: r["segsnr"])

    snrs = [r["snr"] for r in rows if math.isfinite(r["snr"])]
    segs = [r["segsnr"] for r in rows if math.isfinite(r["segsnr"])]
    print(f"samples={len(rows)}  looped={sum(r['loop'] for r in rows)}  decimated={sum(r['decim'] for r in rows)}")
    if snrs:
        print(f"SNR dB     mean={sum(snrs)/len(snrs):6.2f}  min={min(snrs):6.2f}  max={max(snrs):6.2f}")
    if segs:
        print(f"segSNR dB  mean={sum(segs)/len(segs):6.2f}  min={min(segs):6.2f}  max={max(segs):6.2f}")
    print(f"\nworst {args.worst} by segSNR (key=abs tbl offset):")
    print(f"  {'key':>8}  {'len':>7}  {'segSNR':>7}  {'SNR':>7}  {'peak':>6}  loop decim banks")
    for r in rows[: args.worst]:
        print(f"  {r['key']:>8x}  {r['n']:>7}  {r['segsnr']:>7.2f}  {r['snr']:>7.2f}  {r['peak']:>6}  "
              f"{int(r['loop'])}    {int(r['decim'])}     {r['nbanks']}")
        tag = f"{r['key']:08x}"
        _write_wav(os.path.join(args.out, f"{tag}_ref.wav"), r["ref"], args.rate)
        _write_wav(os.path.join(args.out, f"{tag}_adpcm.wav"), r["test"], args.rate)
        _write_wav(os.path.join(args.out, f"{tag}_diff.wav"),
                   [a - b for a, b in zip(r["ref"], r["test"])], args.rate)
    print(f"\nWAVs (ref/adpcm/diff) for worst {args.worst} -> {args.out}")


if __name__ == "__main__":
    main()
