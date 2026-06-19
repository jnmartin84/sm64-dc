#!/usr/bin/env python3
"""
Full per-sample transcode: VADPCM -> PCM16 -> (2x decimate if >64K) -> AICA
sample, producing the final sample bytes + descriptor. Shared by the build-time
emitter.

Output format is chosen per sample by measured quality:
  * Yamaha ADPCM (beam-search encoder, exact-decoder analysis-by-synthesis) for
    samples that round-trip cleanly.
  * PCM for samples whose ADPCM SNR falls below PROMOTE_SNR_DB -- these are the
    content-ceiling cases (loud/HF) that no 4-bit encoder can represent. 16-bit
    if short (<= PCM16_MAX_SAMPLES), else 8-bit, to cap ARAM/pool cost. fmt holds
    the AICA_SM_* code so the runtime passes it straight to the channel.

Tunables via env (AICA_PROMOTE_SNR_DB / AICA_PCM16_MAX_SAMPLES / AICA_BEAM).

Decimation is scipy-optional: scipy.signal.resample_poly is used when available
(the output that was ear-checked/approved), else a stdlib windowed-sinc FIR so a
clean checkout builds with only stock Python 3.
"""

import array
import hashlib
import math
import os
import struct

try:
    import numpy as np
    from scipy.signal import resample_poly
    _HAVE_SCIPY = True
except Exception:
    _HAVE_SCIPY = False

import vadpcm
import yamaha_adpcm_v2 as ya2

AICA_MAX = 65534

# AICA_SM_* sample-format codes (dc/sound/aica_comm.h).
FMT_PCM16, FMT_PCM8, FMT_ADPCM = 0, 1, 2

PROMOTE_SNR_DB = float(os.environ.get("AICA_PROMOTE_SNR_DB", "18.0"))
PCM16_MAX_SAMPLES = int(os.environ.get("AICA_PCM16_MAX_SAMPLES", "2048"))
BEAM = int(os.environ.get("AICA_BEAM", "32"))

# Samples the game pitches beyond the AICA's ADPCM +1-octave playback limit
# (AICA_E.pdf: "PCM enables -8..+7 octaves; ADPCM ... is limited to +1 octave").
# Those notes pitch-clamp on ADPCM (the rising-arpeggio "weird jingle"), so they
# MUST be PCM regardless of SNR. Keyed by absolute tbl offset; identified from
# runtime nf telemetry (AICA_DEBUG note-start log -> nf > 2.0). Tuning is NOT a
# proxy -- the high pitch comes from sequence transposition, not instrument tuning.
# Samples the game pitches above nf=2.0 (+1 octave), found via the AICA_OCTAVE_LOG
# sweep; ADPCM pitch-clamps past +1 octave, so these must be PCM regardless of SNR.
# (Cutoff 2.0: the "fine" group topped out at nf=1.888, a clean gap below.)
FORCE_PCM_KEYS = {int(k, 16) for k in os.environ.get("AICA_FORCE_PCM_KEYS", "BEC60 86A0 18B570 187110 BC130 1AD350 1DBF50 127C80 132150 153E0 19CAB0 158E70 179810").replace(",", " ").split()}


# --- beam-encode result cache --------------------------------------------------
# The beam search is the only expensive step and is deterministic in (pcm, BEAM,
# encoder version). Cache its output so re-runs (e.g. tweaking FORCE_PCM_KEYS,
# which only changes the cheap PCM-vs-ADPCM decision) don't re-encode unchanged
# samples. Keyed by content hash; safe under multiprocessing (atomic replace).
# Bump ENC_VERSION whenever yamaha_adpcm_v2.encode changes.
ENC_VERSION = b"ya2-beam-1"
_CACHE_DIR = os.environ.get("AICA_ENCODE_CACHE",
                            os.path.join(os.path.dirname(os.path.abspath(__file__)), ".encode_cache"))


def beam_encode_cached(pcm):
    """ya2.encode(pcm, beam=BEAM) with an on-disk cache. Returns (adpcm_bytes, n)."""
    h = hashlib.sha1(ENC_VERSION)
    h.update(b"|beam=%d|" % BEAM)
    h.update(array.array("i", pcm).tobytes())
    path = os.path.join(_CACHE_DIR, h.hexdigest())
    try:
        with open(path, "rb") as f:
            n = struct.unpack("<I", f.read(4))[0]
            return f.read(), n
    except OSError:
        pass
    adpcm, n = ya2.encode(pcm, beam=BEAM)
    os.makedirs(_CACHE_DIR, exist_ok=True)
    tmp = "%s.tmp%d" % (path, os.getpid())   # per-pid tmp -> atomic under multiprocessing
    with open(tmp, "wb") as f:
        f.write(struct.pack("<I", n))
        f.write(adpcm)
    os.replace(tmp, path)
    return adpcm, n


def _snr_db(ref, test):
    sig = err = 0.0
    for a, b in zip(ref, test):
        sig += float(a) * a
        err += float(a - b) * (a - b)
    if err == 0.0:
        return 99.0
    if sig == 0.0:
        return -99.0
    return 10.0 * math.log10(sig / err)


def _pcm16_bytes(pcm):
    return struct.pack(f"<{len(pcm)}h", *(_clamp16(v) for v in pcm))


def _pcm8_bytes(pcm):
    # AICA 8-bit is signed linear (top 8 bits of the 16-bit sample), rounded.
    out = bytearray(len(pcm))
    for i, v in enumerate(pcm):
        q = (v + 128) >> 8
        q = -128 if q < -128 else 127 if q > 127 else q
        out[i] = q & 0xFF
    return bytes(out)


def _clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def _decimate2(pcm):
    """Anti-aliased 2x downsample. scipy if present (matches approved output);
    else a 23-tap Hamming-windowed-sinc lowpass (fc=0.25) + take every 2nd."""
    if _HAVE_SCIPY:
        dec = resample_poly(np.asarray(pcm, dtype=np.float64), 1, 2)
        return [_clamp16(int(round(v))) for v in dec]
    N, fc = 23, 0.25
    mid = (N - 1) // 2
    h = []
    for i in range(N):
        x = i - (N - 1) / 2.0
        s = 2 * fc if x == 0 else math.sin(2 * math.pi * fc * x) / (math.pi * x)
        w = 0.54 - 0.46 * math.cos(2 * math.pi * i / (N - 1))
        h.append(s * w)
    g = sum(h)
    h = [c / g for c in h]
    L = len(pcm)
    out = []
    for n in range(0, L, 2):
        acc = 0.0
        for k in range(N):
            idx = n + k - mid
            idx = 0 if idx < 0 else (L - 1 if idx >= L else idx)
            acc += pcm[idx] * h[k]
        out.append(_clamp16(int(round(acc))))
    return out


def transcode_sample(s):
    """s: albank_parse.Sample (parsed with_data=True). Returns descriptor dict.
    Beam-encodes to ADPCM, then promotes to PCM if the ADPCM SNR is below
    PROMOTE_SNR_DB (16-bit when short, else 8-bit)."""
    pcm = vadpcm.decode(s.data, s.codec, s.order, s.npredictors, s.book)
    shift = 0
    loop_start, loop_end = s.loop_start, s.loop_end

    if len(pcm) > AICA_MAX:
        # 2x trick: anti-aliased decimate by 2, play back at half freq at runtime.
        pcm = _decimate2(pcm)
        shift = 1
        loop_start //= 2
        loop_end //= 2

    adpcm, n = beam_encode_cached(pcm)
    assert n <= AICA_MAX, (s.bank, s.addr, n)
    snr = _snr_db(pcm[:n], ya2.decode(adpcm, n)[:n])

    if snr < PROMOTE_SNR_DB or s.addr in FORCE_PCM_KEYS:
        if n <= PCM16_MAX_SAMPLES:
            fmt, data = FMT_PCM16, _pcm16_bytes(pcm[:n])
        else:
            fmt, data = FMT_PCM8, _pcm8_bytes(pcm[:n])
    else:
        fmt, data = FMT_ADPCM, adpcm

    if not s.has_loop:
        # one-shot: AICA still needs loopend = sample count as the play length
        loop_start, loop_end = 0, n

    return {
        "bank": s.bank,
        "addr": s.addr,
        "data": data,
        "fmt": fmt,
        "snr": snr,
        "nsamples": n,
        "loop": bool(s.has_loop),
        "loop_start": loop_start,
        "loop_end": min(loop_end, n),
        "downsample_shift": shift,
    }
