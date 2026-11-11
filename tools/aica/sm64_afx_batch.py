#!/usr/bin/env python3
"""Convert the recorder's logs into staged AICAflow assets.

    tools/aica/sm64_afx_batch.py build/audio_rec/logs --build build/us_dc --stage build/afx/stage

For each rec_seqXX.bin: a song with two recorded passes (two sequence jumps)
becomes a looping flow, seqXX; one without a jump (a jingle) stays one-shot.
For each rec_sfx_XXXXXXXX.bin: a one-shot sound, staged as sfx_<bank and id>
(the bits & 0xF0FF0000 key src/pc/audio/sm64_aicaflow.c looks up). Each result
is checked against its recording (sm64_afx_verify.py); a summary table follows.
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sm64_afx as conv


# The songs whose channels the game fades or starts apart, as sequence channel masks: sMusicDynamics
# (src/audio/external.c) and SA's variation of the water song. Lane n + 1 holds mask n, lane 0 the
# rest; src/pc/audio/sm64_aicaflow.c has the same table.
SONG_LANES = {"05": "0x0003,0x0e40,0xc000", "0A": "0x0070", "0C": "0x0100,0x0008"}


def lane_args(seq):
    return ["--lane-masks", SONG_LANES[seq]] if seq in SONG_LANES else []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--build", required=True, help="the recording build's us_dc directory")
    ap.add_argument("--stage", required=True)
    ap.add_argument("--work", default="build/afx/work")
    # Tolerance 2 (0.75 dB): on hardware (2026-10-27 A/B of all songs against the exact
    # conversion) as close in level and spectrum as tolerance 1, and the songs 37% smaller; the
    # sounds (2026-10-28, 40 of them) within the noise of the exact ones played twice, 8% smaller.
    ap.add_argument("--hw-env", metavar="TOL", default="2",
                    help="passed to sm64_afx.py: fades on the AICA envelope (default 2; 0 = off)")
    ap.add_argument("--afx-sm64", help="passed to sm64_afx.py")
    args = ap.parse_args()
    run, stage, work = Path(args.run), Path(args.stage) / "afx", Path(args.work)
    stage.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    table = Path(args.build) / "sound" / "aica_sample_table.c"
    pool = Path(args.build) / "sound" / "aica_pool.bin"
    rows = []
    for log in sorted(run.glob("rec_*.bin")):
        m = re.match(r"rec_(seq([0-9A-F]{2})|sfx_([0-9A-F]{8}))\.bin", log.name)
        if not m:
            continue
        upf, waves, records = conv.read_log(log)
        continuous = False
        starts = sum(1 for r in records if r[1] == conv.START)
        targets = [r[6] for r in records if r[1] == conv.SEQ_JUMP and r[2] == 0]
        jumps = max((targets.count(t) for t in set(targets)), default=0)   # the loop's jump back
        if m.group(2):
            name = f"seq{m.group(2)}"
            loop = jumps >= 2
        else:
            bits = int(m.group(3), 16)
            name = f"sfx_{bits & 0xF0FF0000:08X}"
            loop = False
            continuous = not bits & 0x80   # the game keeps requesting it (SOUND_DISCRETE clear)
        if not starts:
            rows.append((name, "silent: nothing played", "", ""))
            continue
        out = work / name
        cmd = [sys.executable, str(HERE / "sm64_afx.py"), str(log), "--table", str(table), "--pool", str(pool),
               "--out", str(out)] + (["--loop"] if loop else ["--tail", "2"]) + (["--continuous"] if continuous else []) + (lane_args(m.group(2)) if m.group(2) else ["--lanes", "--full-send"])
        cmd += (["--hw-env", args.hw_env] if args.hw_env not in (None, "0") else []) + (["--afx-sm64", args.afx_sm64] if args.afx_sm64 else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            rows.append((name, "conversion failed", "", r.stdout.strip().splitlines()[-1:] + r.stderr.strip().splitlines()[-1:]))
            continue
        v = subprocess.run([sys.executable, str(HERE / "sm64_afx_verify.py"), str(out.with_suffix(".afx")),
                            "--tolerance", "10"], capture_output=True, text=True)
        first = next((l for l in v.stdout.splitlines() if l.startswith("pass 1:")), "")
        dis = re.search(r"disagreement (\d+) ticks \(([\d.]+)%\)", first)
        meta = json.loads(out.with_suffix(".json").read_text())
        for ext in (".afb", ".afx"):
            shutil.copy(out.with_suffix(ext), stage / (name + ext))
        afb, afx = out.with_suffix(".afb").stat().st_size, out.with_suffix(".afx").stat().st_size
        looped = meta["loop_tick"] is not None
        kind = f"loop at {meta['loop_tick']}, {meta['end']} ticks" if looped else f"one-shot, {meta['end']} ticks"
        rows.append((name, kind, f"{afb + afx:,} B", f"pass 1 off {dis.group(1)} ticks ({dis.group(2)}%)" if dis else first))
    w = max(len(r[0]) for r in rows) if rows else 8
    for name, kind, size, check in rows:
        print(f"{name:<{w}}  {kind:<32} {size:>10}  {check}")


if __name__ == "__main__":
    main()
