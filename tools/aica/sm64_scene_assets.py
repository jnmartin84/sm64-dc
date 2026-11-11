#!/usr/bin/env python3
"""What each level can play, from the game's own data, and what that costs in AICA RAM.

    tools/aica/sm64_scene_assets.py [--stage build/afx/stage] [--json out.json]

A level's objects come from its level script (OBJECT lines), its areas' macro objects and special
objects, and everything those spawn (SPAWN_OBJ / SPAWN_CHILD in behavior scripts, spawn_object*(...,
bhvX) in the behavior's C file: a name passed to a spawn call, not any mention), taken transitively. A behavior can play the SOUND_* and SEQ_*
names of the C files its CALL_NATIVE functions live in. Everything the rest of src/game plays
(Mario, camera, menus, level logic, generic object helpers) is global: possible in every level,
except songs: those the general code plays belong to the levels SONG_RULES gives them to (the
power-up and metal-cap songs to levels whose exclamation boxes hold those caps). The result is an
over-estimate: a file's every sound counts for each of its behaviors.

With --stage, the cost of each level's set in AICA RAM: its flows plus its samples, each sample
once (as a scene bank would hold them), shown with and without the global set.
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def read(path):
    return (ROOT / path).read_text(errors="replace")


def defines(src, pattern):
    return {m[1]: m[0] for m in re.finditer(pattern, src)}


def sound_keys():
    """SOUND_* name -> asset key (bank << 28 | id << 16), from include/audio_defines.h."""
    out = {}
    src = read("include/audio_defines.h")
    for m in re.finditer(r"#define\s+(SOUND_\w+)\s+SOUND_ARG_LOAD\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)", src):
        try:
            out[m[1]] = int(m[2], 0) << 28 | int(m[4], 0) << 16
        except ValueError:
            pass
    return out


def seq_ids():
    names = re.findall(r"\b(SEQ_\w+),\s*//\s*0x([0-9A-Fa-f]+)", read("include/seq_ids.h"))
    return {n: int(v, 16) for n, v in names}


def behaviors():
    """bhv name -> (native functions, spawned behaviors) from data/behavior_data.c."""
    src = read("data/behavior_data.c")
    out = {}
    for m in re.finditer(r"const BehaviorScript (bhv\w+)\[\] = \{(.*?)\n\};", src, re.S):
        body = m[2]
        natives = set(re.findall(r"CALL_NATIVE\((\w+)\)", body))
        spawns = set(re.findall(r"\b(bhv\w+)\b", body))
        jumps = set(re.findall(r"GOTO\((bhv\w+)", body))
        out[m[1]] = (natives, spawns | jumps)
    return out


def c_files():
    """function name -> file, and per file its SOUND_/SEQ_ names and spawned behaviors."""
    func_file, file_info = {}, {}
    files = sorted(glob.glob(str(ROOT / "src/game/**/*.c"), recursive=True))
    for f in files:
        src = Path(f).read_text(errors="replace")
        rel = os.path.relpath(f, ROOT)
        for fn in re.findall(r"^\w[\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*\{", src, re.M):
            func_file.setdefault(fn, rel)
        # Behaviors a file spawns: names passed to a spawn call (a plain mention, such as a check of
        # which behavior spawned an object, spawns nothing).
        spawned = set()
        for call in re.findall(r"\b\w*spawn\w*\s*\(([^;]*)", src):
            spawned |= set(re.findall(r"\b(bhv\w+)\b", call))
        file_info[rel] = (set(re.findall(r"\b(SOUND_[A-Z0-9_]+)\b", src)),
                          set(re.findall(r"\b(SEQ_[A-Z0-9_]+)\b", src)),
                          spawned)
    return func_file, file_info


def level_behaviors():
    """level dir -> behaviors placed in it (script, macro and special objects)."""
    macro = read("include/macro_presets.h")
    macro_names = re.findall(r"^\s*(macro_\w+)", read("include/macro_preset_names.h"), re.M)
    macro_bhv = re.findall(r"\{\s*(bhv\w+|NULL)\s*,", macro.split("MacroObjectPresets")[1])
    macro_map = dict(zip(macro_names, macro_bhv))
    special_names = re.findall(r"^\s*(special_\w+)\s*(?:=\s*(0x[0-9A-Fa-f]+|\d+))?", read("include/special_preset_names.h"), re.M)
    ids, n = {}, 0
    for name, val in special_names:
        n = int(val, 0) if val else n
        ids[name] = n
        n += 1
    special_by_id = {int(m[1], 16): m[2] for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+)\s*,[^,]*,[^,]*,[^,]*,\s*(bhv\w+|NULL)\s*\}", read("include/special_presets.h"))}
    out = collections.defaultdict(set)
    for d in sorted(p for p in (ROOT / "levels").iterdir() if p.is_dir()):
        lvl = d.name
        for f in glob.glob(str(d / "**" / "*.c"), recursive=True):
            src = Path(f).read_text(errors="replace")
            out[lvl] |= set(re.findall(r"/\*beh\*/\s*(bhv\w+)", src))
            for name in re.findall(r"MACRO_OBJECT\w*\(/\*preset\*/\s*(macro_\w+)", src):
                if macro_map.get(name, "NULL") != "NULL":
                    out[lvl].add(macro_map[name])
            for name in re.findall(r"SPECIAL_OBJECT\w*\(/\*preset\*/\s*(special_\w+)", src):
                b = special_by_id.get(ids.get(name, -1))
                if b and b != "NULL":
                    out[lvl].add(b)
    return out


def closure(start, bhv_table, func_file, file_info):
    """All behaviors reachable from start, and the files they run."""
    seen, files, todo = set(), set(), list(start)
    while todo:
        b = todo.pop()
        if b in seen or b not in bhv_table:
            continue
        seen.add(b)
        natives, spawns = bhv_table[b]
        todo += [s for s in spawns if s not in seen]
        for fn in natives:
            f = func_file.get(fn)
            if f:
                files.add(f)
                todo += [s for s in file_info[f][2] if s not in seen]
    return seen, files


# Sounds played as name + (n << 16), and how many members each family has.
SOUND_FAMILIES = {
    "SOUND_MENU_COLLECT_RED_COIN": 8, "SOUND_MENU_COLLECT_SECRET": 5, "SOUND_MARIO_YAH_WAH_HOO": 3,
    "SOUND_MARIO_YAHOO_WAHA_YIPPEE": 5, "SOUND_MARIO_PANTING": 3,
}

# Songs the general game code plays, by where they can play. "*" is every level; "caps:wing" etc.
# are the levels with that cap in an exclamation box.
SONG_RULES = {
    "SEQ_EVENT_CUTSCENE_COLLECT_STAR": "*", "SEQ_EVENT_CUTSCENE_STAR_SPAWN": "*", "SEQ_EVENT_SOLVE_PUZZLE": "*",
    "SEQ_EVENT_POWERUP": "caps:wing caps:vanish", "SEQ_EVENT_METAL_CAP": "caps:metal",
    "SEQ_LEVEL_INSIDE_CASTLE": "castle_inside", "SEQ_EVENT_ENDLESS_STAIRS": "castle_inside",
    "SEQ_EVENT_TOAD_MESSAGE": "castle_inside",
    "SEQ_EVENT_CUTSCENE_INTRO": "castle_grounds", "SEQ_EVENT_PEACH_MESSAGE": "castle_grounds castle_inside",
    "SEQ_EVENT_CUTSCENE_LAKITU": "castle_grounds",
    "SEQ_EVENT_CUTSCENE_COLLECT_KEY": "bowser_1 bowser_2", "SEQ_EVENT_CUTSCENE_VICTORY": "bowser_3",
    "SEQ_EVENT_CUTSCENE_CREDITS": "ending", "SEQ_EVENT_CUTSCENE_ENDING": "ending",
    "SEQ_MENU_TITLE_SCREEN": "intro menu", "SEQ_MENU_FILE_SELECT": "menu",
    # The act select runs in the course's own level (gCurrLevelNum is the course's by then).
    "SEQ_MENU_STAR_SELECT": "menu bob wf jrb ccm bbh hmc lll ssl ddd sl wdw ttm thi ttc rr",
    # Played through wrappers (play_star_fanfare, play_race_fanfare), so no SEQ_ name in the
    # callers' files: the course-complete fanfare where a course is left to, the race start.
    "SEQ_EVENT_HIGH_SCORE": "castle_inside castle_grounds castle_courtyard",
    "SEQ_EVENT_RACE": "bob thi ccm",
}
CAP_BOXES = {"macro_box_wing_cap": "wing", "macro_box_metal_cap": "metal", "macro_box_vanish_cap": "vanish"}


def level_caps(lvl):
    """The caps a level's exclamation boxes hold."""
    caps = set()
    for f in glob.glob(str(ROOT / f"levels/{lvl}/**/*.c"), recursive=True):
        src = Path(f).read_text(errors="replace")
        caps |= {CAP_BOXES[m] for m in re.findall(r"\b(macro_box_\w+_cap)\b", src) if m in CAP_BOXES}
        for param in re.findall(r"/\*behParam\*/\s*0x([0-9A-Fa-f]{8}),\s*/\*beh\*/\s*bhvExclamationBox\b", src):
            caps |= {["wing", "metal", "vanish"][b] for b in [int(param[2:4], 16)] if b < 3}
    return caps


def ruled_songs(lvl):
    caps = level_caps(lvl)
    out = set()
    for song, where in SONG_RULES.items():
        for w in where.split():
            if w == "*" or w == lvl or (w.startswith("caps:") and w[5:] in caps):
                out.add(song)
    return out


def asset_cost(stems, stage):
    """Flow image bytes, and unique sample bytes (by content), of the staged assets in stems."""
    flows, samples = 0, {}
    for stem in stems:
        afx, afb = stage / f"{stem}.afx", stage / f"{stem}.afb"
        if not afx.exists():
            continue
        x = afx.read_bytes()
        flows += struct.unpack_from("<I", x, 20)[0]
        b = afb.read_bytes()
        off, size = struct.unpack_from("<II", b, 16)
        pay = b[off:off + size]
        ro, rc = struct.unpack_from("<II", x, 48)
        for i in range(rc):
            _, so, sb = struct.unpack_from("<III", x, ro + 12 * i)
            samples[hashlib.sha1(pay[so:so + sb]).hexdigest()] = (sb + 31) & ~31
    return flows, samples


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", default="build/afx/stage")
    ap.add_argument("--json")
    args = ap.parse_args()
    keys, seqs = sound_keys(), seq_ids()
    bhv_table = behaviors()
    func_file, file_info = c_files()
    behavior_files = {f for f in file_info if "/behaviors/" in f or f.endswith("behavior_actions.c")}
    global_files = set(file_info) - behavior_files
    g_sounds = set().union(*(file_info[f][0] for f in global_files))
    # The act select (src/menu/star_select.c) runs in every course's own level.
    g_sounds |= set(re.findall(r"\b(SOUND_[A-Z0-9_]+)\b", read("src/menu/star_select.c")))
    g_seqs = {q for q, where in SONG_RULES.items() if where == "*"}
    stage = ROOT / args.stage / "afx"

    def stems(sounds, songs):
        out = {f"sfx_{keys[s]:08X}" for s in sounds if s in keys}
        # Mario's terrain sounds are played with m->terrainSoundAddend added: SOUND_TERRAIN_* << 16.
        out |= {f"sfx_{keys[s] + (t << 16):08X}" for s in sounds if s in keys and "_TERRAIN_" in s for t in range(8)}
        # Others are picked as name + (n << 16): the next red coin's or secret's note, Mario's random voices.
        out |= {f"sfx_{keys[s] + (t << 16):08X}" for s in sounds if s in keys for t in range(SOUND_FAMILIES.get(s, 0))}
        out |= {f"seq{seqs[q]:02X}" for q in songs if q in seqs and seqs[q] != 0}
        return out

    g_stems = stems(g_sounds, g_seqs)
    g_flows, g_samples = asset_cost(g_stems, stage)
    print(f"global: {len(g_stems)} assets, flows {g_flows / 1024:.0f} KB + samples {sum(g_samples.values()) / 1024:.0f} KB")
    result = {"global": sorted(g_stems), "levels": {}, "music": {}}
    rows = []
    # The title screen, file select and star select (level 1: levels/intro, levels/menu) are run
    # by src/menu and goddard's Mario head, outside src/game.
    menu_sounds, menu_seqs = set(), set()
    for f in glob.glob(str(ROOT / "src/menu/**/*.c"), recursive=True) + glob.glob(str(ROOT / "src/goddard/**/*.c"), recursive=True):
        src = Path(f).read_text(errors="replace")
        menu_sounds |= set(re.findall(r"\b(SOUND_[A-Z0-9_]+)\b", src))
        menu_seqs |= set(re.findall(r"\b(SEQ_[A-Z0-9_]+)\b", src))
    for lvl, placed in sorted(level_behaviors().items()):
        _, files = closure(placed, bhv_table, func_file, file_info)
        sounds = set().union(set(), *(file_info[f][0] for f in files))
        songs = set().union(set(), *(file_info[f][1] for f in files))
        songs |= set(re.findall(r"\b(SEQ_[A-Z_]+)\b", read(f"levels/{lvl}/script.c")) if (ROOT / f"levels/{lvl}/script.c").exists() else set())
        songs |= ruled_songs(lvl)
        if lvl in ("intro", "menu"):
            sounds |= menu_sounds
            songs |= menu_seqs
        own = stems(sounds, songs) - g_stems
        result["levels"][lvl] = sorted(own)
        # The level's own music (its script's, and the menus' for the title screen): what a pack
        # loads first, so that when AICA RAM runs short an event song is what is left out.
        script = ROOT / f"levels/{lvl}/script.c"
        music = set(re.findall(r"\b(SEQ_[A-Z_]+)\b", script.read_text())) if script.exists() else set()
        if lvl in ("intro", "menu"):
            music |= menu_seqs | {q for q, where in SONG_RULES.items() if lvl in where.split()}
        # The race fanfare plays as the race's song loads, so it must not sit in the room that takes.
        if "SEQ_EVENT_RACE" in SONG_RULES and lvl in SONG_RULES["SEQ_EVENT_RACE"].split():
            music.add("SEQ_EVENT_RACE")
        result["music"][lvl] = sorted(stems(set(), music))
        flows, samples = asset_cost(own, stage)
        total = dict(g_samples, **samples)
        rows.append((lvl, len(own), flows, sum(samples.values()), g_flows + flows, sum(total.values())))
    print(f"\n{'level':18s} {'own':>4s} {'own flows':>10s} {'own smp':>9s} {'scene total (with global)':>27s}")
    for lvl, n, fl, sm, tf, ts in rows:
        print(f"{lvl:18s} {n:4d} {fl / 1024:8.0f}KB {sm / 1024:7.0f}KB   {tf / 1024:6.0f}KB flows + {ts / 1024:5.0f}KB samples = {(tf + ts) / 1024:5.0f}KB")
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
