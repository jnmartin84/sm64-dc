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

# --- optional baked-in software reverb (AICA_REVERB=1) -------------------------
# A light Freeverb (Schroeder comb+allpass) convolved into each sample's PCM
# BEFORE ADPCM/PCM encode, so the dry hardware path carries a touch of room.
# This is a blunt tool: the reverb time repitches with the note (no fix), and
# looped samples can't have a decaying tail, so the loop region is run to steady
# state and the head crossfaded in (REVERB_XFADE). One-shots get a real tail,
# capped to AICA_MAX. Params are intentionally fixed -- edit the constants below
# to taste; keep WET low and DRY < 1 for headroom (AICA has no overflow clamp).
REVERB = os.environ.get("AICA_REVERB", "0") not in ("0", "", "false", "False")
REVERB_RATE = 32000            # nominal playback rate used to scale delay lengths
REVERB_DRY = 0.84              # dry gain (<1 for summing headroom)
REVERB_WET = 0.26              # wet gain -- keep light
REVERB_INPUT_GAIN = 0.015      # Freeverb pre-scale into the comb bank
REVERB_ROOMSIZE = 0.55         # -> comb feedback = roomsize*0.28 + 0.7
REVERB_DAMPING = 0.5           # -> comb lowpass damp1 = damping*0.4
REVERB_FEEDBACK = REVERB_ROOMSIZE * 0.28 + 0.7
REVERB_DAMP1 = REVERB_DAMPING * 0.4
REVERB_XFADE = 96              # head->steady-loop crossfade length (samples)
REVERB_PREWARM = 64000         # loop samples run to reach steady state (~>RT60)
REVERB_MAX_PREWARM = 96000     # hard cap on warmup work for tiny loops
REVERB_TAIL_CAP = 20000        # hard cap on one-shot tail appended (samples)
REVERB_TAIL_FLOOR = 0.01       # stop the tail once it decays this far below body peak (-40 dB)
REVERB_TAIL_QUIET = 256        # ...for this many consecutive samples

# Samples the game pitches beyond the AICA's ADPCM +1-octave playback limit
# (AICA_E.pdf: "PCM enables -8..+7 octaves; ADPCM ... is limited to +1 octave").
# Those notes pitch-clamp on ADPCM (the rising-arpeggio "weird jingle"), so they
# MUST be PCM regardless of SNR. Keyed by absolute tbl offset; identified from
# runtime nf telemetry (AICA_DEBUG note-start log -> nf > 2.0). Tuning is NOT a
# proxy -- the high pitch comes from sequence transposition, not instrument tuning.
# Samples the game pitches above nf=2.0 (+1 octave), found via the AICA_OCTAVE_LOG
# sweep; ADPCM pitch-clamps past +1 octave, so these must be PCM regardless of SNR.
# (Cutoff 2.0: the "fine" group topped out at nf=1.888, a clean gap below.)
FORCE_PCM_KEYS = {int(k, 16) for k in os.environ.get("AICA_FORCE_PCM_KEYS", "BEC60 86A0 18B570 187110 BC130 1AD350 1DBF50 127C80 132150 153E0 19CAB0 158E70 179810 198CB0 B9DF0 1CCE30 1C41D0").replace(",", " ").split()}


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


class _Comb:
    """Lowpass-feedback comb filter (one Freeverb voice)."""
    __slots__ = ("buf", "idx", "store", "fb", "d1", "d2")

    def __init__(self, size, fb, d1):
        self.buf = [0.0] * size
        self.idx = 0
        self.store = 0.0
        self.fb = fb
        self.d1 = d1
        self.d2 = 1.0 - d1

    def process(self, x):
        y = self.buf[self.idx]
        self.store = y * self.d2 + self.store * self.d1
        self.buf[self.idx] = x + self.store * self.fb
        self.idx += 1
        if self.idx >= len(self.buf):
            self.idx = 0
        return y


class _Allpass:
    __slots__ = ("buf", "idx", "fb")

    def __init__(self, size, fb):
        self.buf = [0.0] * size
        self.idx = 0
        self.fb = fb

    def process(self, x):
        bufout = self.buf[self.idx]
        y = bufout - x
        self.buf[self.idx] = x + bufout * self.fb
        self.idx += 1
        if self.idx >= len(self.buf):
            self.idx = 0
        return y


class _Freeverb:
    """Mono Freeverb: 8 parallel combs -> 4 series allpasses. Delay tunings are
    the classic 44.1 kHz values scaled to REVERB_RATE."""
    _COMB = (1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617)
    _ALLP = (556, 441, 341, 225)

    def __init__(self, rate):
        sc = rate / 44100.0
        self.combs = [_Comb(max(1, int(round(t * sc))), REVERB_FEEDBACK, REVERB_DAMP1)
                      for t in self._COMB]
        self.allps = [_Allpass(max(1, int(round(t * sc))), 0.5) for t in self._ALLP]

    def process(self, x):
        inp = x * REVERB_INPUT_GAIN
        out = 0.0
        for c in self.combs:
            out += c.process(inp)
        for a in self.allps:
            out = a.process(out)
        return out


def _apply_reverb(pcm, has_loop, loop_start, loop_end):
    """Bake a light reverb into pcm. One-shots gain a decaying tail (capped to
    AICA_MAX); looped samples run the loop region to steady state so it stays
    periodic across the seam, then crossfade the played-once head into it.
    Returns (pcm, loop_start, loop_end)."""
    rv = _Freeverb(REVERB_RATE)

    if not has_loop:
        out = [_clamp16(int(round(x * REVERB_DRY + rv.process(x) * REVERB_WET)))
               for x in pcm]
        floor = max(1.0, max((abs(v) for v in out), default=0) * REVERB_TAIL_FLOOR)
        room = max(0, AICA_MAX - len(out))
        quiet = 0
        for _ in range(min(REVERB_TAIL_CAP, room)):
            w = rv.process(0.0) * REVERB_WET
            out.append(_clamp16(int(round(w))))
            if abs(w) < floor:
                quiet += 1
                if quiet >= REVERB_TAIL_QUIET:
                    break
            else:
                quiet = 0
        return out, loop_start, loop_end

    P = loop_end - loop_start
    if P <= 0:
        out = [_clamp16(int(round(x * REVERB_DRY + rv.process(x) * REVERB_WET)))
               for x in pcm]
        return out, loop_start, loop_end

    head = pcm[:loop_start]
    loop = pcm[loop_start:loop_end]
    head_out = [x * REVERB_DRY + rv.process(x) * REVERB_WET for x in head]

    reps = max(2, REVERB_PREWARM // P + 1)
    reps = min(reps, max(2, REVERB_MAX_PREWARM // P))
    for _ in range(reps - 1):
        for x in loop:
            rv.process(x)
    loop_out = [x * REVERB_DRY + rv.process(x) * REVERB_WET for x in loop]

    # Smooth the one-time head->loop entry: the steady loop is already seamless at
    # its own wrap (loop_end->loop_start) by periodicity, so we only need to morph
    # the tail of the played-once head into the steady loop's natural pre-roll.
    xf = min(REVERB_XFADE, len(head_out), P)
    base = len(head_out) - xf
    for i in range(xf):
        t = (i + 1) / (xf + 1)
        head_out[base + i] = (1.0 - t) * head_out[base + i] + t * loop_out[P - xf + i]

    out = [_clamp16(int(round(v))) for v in head_out]
    out.extend(_clamp16(int(round(v))) for v in loop_out)
    return out, loop_start, loop_start + P


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

    if REVERB:
        pcm, loop_start, loop_end = _apply_reverb(pcm, s.has_loop, loop_start, loop_end)

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
