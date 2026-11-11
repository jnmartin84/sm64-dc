#!/usr/bin/env python3
"""Scene packs with one shared sample bank each (pack version 3), from sm64_scene_assets.py's lists.

    tools/aica/sm64_scene_assets.py --json build/afx/scene_assets.json
    tools/aica/sm64_scene_banks.py build/afx/scene_assets.json --stage build/afx/stage

Every staged asset carries its own AFB with copies of the samples it uses, so samples that songs
and sounds share were loaded many times over. Here each pack holds one AFB with every sample its
flows use, once, and the flows are rewritten to bind to it: each relocation's sample offset, the
address in the setup it names (CONTROL's low 7 bits and SAMPLE_LOW, which the loader checks
against the relocation), the bank identity and the control id. AICAflow's runtime compares bank
identities from the headers only, so a bank built here is as good as one from AICAforge.

scene_core.smp holds the global set, read once; scene_LL.smp (LL = gCurrLevelNum, decimal) the
level's own assets. A level's bank holds only the samples the core bank does not: a level flow
whose samples are all in the core bank binds to that instead, and one with samples in both plays
from both (AICAflow's AFX_FLAG_BANKS, firmware ABI 13), the level bank in slot 0 and the core
bank in slot 1.

Each level pack is fitted to the AICA RAM left after the core pack and the reverb ring (--arena-kb,
--reverb-kb): its music, then its sounds (smallest flow first), then other songs (jingles, event
music). A song left out is loaded from its own files when it plays, so the
songs packed are chosen to leave room for the largest left out where that lets more of them play
(see fit). The bank holds only the samples of what is in the pack.

Pack format (little-endian): "SMPK", version 3, entry count, bank offset, bank bytes (a whole
AFB file); per entry a 24-byte NUL-padded stem, the bank it binds (0 = this pack's, 1 = the core
pack's, 2 = both), the offset and size of its AFX in the file, and a reserved word; files 32-byte
aligned. Version 2 is the same without entries of bank 2.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MAGIC = b"SMPK"
ENTRY = struct.Struct("<24s4I")
SETUP_BYTES = 36
HEADER_BYTES = 80
FLAG_BANKS = 64          # AICAflow format.h AFX_FLAG_BANKS
SLOT_SHIFT = 21          # AFX_BANK_SLOT_SHIFT: bits 21-22 of a sample address are its bank slot
BANK_TABLE_BYTES = 16    # AFX_BANK_TABLE_BYTES


def control_id(image):
    """AICAflow's afx_control_id: FNV-1a of the upload image, never zero."""
    h = 2166136261
    for b in image:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h or 1


def level_numbers():
    """level folder -> gCurrLevelNum, in levels/level_defines.h order (LEVEL_NONE is 0)."""
    out, n = {}, 0
    for line in (ROOT / "levels/level_defines.h").read_text().splitlines():
        m = re.match(r"\s*(DEFINE_LEVEL|STUB_LEVEL)\((.*)\)", line)
        if not m:
            continue
        n += 1
        args = [a.strip() for a in m[2].split(",")]
        if m[1] == "DEFINE_LEVEL":
            out[args[3]] = n
    # The title screen, file select and star select run as LEVEL_UNKNOWN_1 (levels/intro, levels/menu).
    out["intro"] = out["menu"] = 1
    return out


PCMS_SHIFT, PCM8, ADPCM = 7, 1, 2    # CONTROL bits 8:7, the sample format
# Opcode -> prefix bytes before a NOTE/PATCH's values (AICAflow codec.h afx_decode_event).
PREFIX = {0x00: 1, 0x13: 1, 0x01: 2, 0x02: 3, 0x03: 5, 0x16: 5, 0x12: 2, 0x10: 8, 0x14: 4, 0x15: 2, 0x17: 2, 0x18: 2, 0x11: 6}
FIXED_MASK = {0x14: 1 << 6 | 1 << 10, 0x15: 1 << 10, 0x17: 1 << 4, 0x18: 1 << 4 | 1 << 10}


def notes_set_sample(afx):
    """Whether any NOTE in the flow's stream gives its own CONTROL or SAMPLE_LOW (then the
    setup's sample format is not the one that plays, and re-encoding a sample needs more)."""
    image, _, stream_off, stream_size = struct.unpack_from("<4I", afx, 16)
    p, end = image + stream_off, image + stream_off + stream_size
    while p < end:
        op = afx[p]
        prefix = PREFIX[op]
        mask = 0
        if op in FIXED_MASK:
            mask = FIXED_MASK[op]
        elif op in (0x10, 0x11):
            mask = struct.unpack_from("<I", afx, p + prefix - 4)[0]
        if op in (0x10, 0x14) and mask & 3:
            return True
        p += prefix + 2 * bin(mask).count("1")
    return False


def adpcm_encode(data):
    """A PCM8 sample's bytes as Yamaha ADPCM, by the build's beam encoder (tools/aica/ya2beam_c)."""
    import ya2beam_c
    pcm = [((v ^ 0x80) - 0x80) << 8 for v in data]
    adpcm, n = ya2beam_c.encode(pcm, beam=32)
    assert n == len(pcm), (n, len(pcm))
    return adpcm


class Asset:
    def __init__(self, stage, stem, adpcm=()):
        self.stem = stem
        self.afx = bytearray((stage / f"{stem}.afx").read_bytes())
        afb = (stage / f"{stem}.afb").read_bytes()
        off, size = struct.unpack_from("<II", afb, 16)
        payload = afb[off:off + size]
        image = struct.unpack_from("<I", self.afx, 16)[0]
        ro, rc = struct.unpack_from("<II", self.afx, 48)
        self.relocs = []
        for i in range(rc):
            pair, so, sb = struct.unpack_from("<III", self.afx, ro + 12 * i)
            data = payload[so:so + sb]
            if hashlib.sha1(data).hexdigest()[:12] in adpcm:
                # Chosen to play as ADPCM: re-encode, and the setup that names it says so.
                control = struct.unpack_from("<H", self.afx, image + pair)[0]
                assert (control >> PCMS_SHIFT) & 3 == PCM8, (stem, hex(control))
                assert not notes_set_sample(self.afx), f"{stem}: a NOTE sets its own sample"
                data = adpcm_cache.get(data) or adpcm_cache.setdefault(data, adpcm_encode(data))
                struct.pack_into("<H", self.afx, image + pair, control & ~(3 << PCMS_SHIFT) | ADPCM << PCMS_SHIFT)
                struct.pack_into("<I", self.afx, ro + 12 * i + 8, len(data))
            self.relocs.append((pair, data))

    def samples(self):
        return {hashlib.sha1(data).digest(): data for _, data in self.relocs}

    def bound(self, offsets, bank_id):
        """The AFX rewritten for a bank: offsets maps sample digest -> offset in it."""
        x = bytearray(self.afx)
        image_off, image_size = struct.unpack_from("<II", x, 16)
        ro, _ = struct.unpack_from("<II", x, 48)
        for i, (pair, data) in enumerate(self.relocs):
            new = offsets[hashlib.sha1(data).digest()]
            struct.pack_into("<I", x, ro + 12 * i + 4, new)
            at = image_off + pair
            control = struct.unpack_from("<H", x, at)[0]
            struct.pack_into("<HH", x, at, (control & ~0x7F) | (new >> 16), new & 0xFFFF)
        struct.pack_into("<II", x, 40, *bank_id)
        struct.pack_into("<I", x, 32, control_id(x[image_off:image_off + image_size]))
        return bytes(x)

    def bound_both(self, level, core):
        """The AFX rebuilt to play from two banks (AFX_FLAG_BANKS, as AICAforge's afx_bank
        --shared): level = (offsets, id) fills slot 0, core = (offsets, id) slot 1, which a
        sample takes when the core bank holds it. The bank list goes after the relocations, and
        the image ends with the zero bank table the loader fills."""
        x = self.afx
        image_off, image_size, stream_off, stream_size = struct.unpack_from("<4I", x, 16)
        ro, rc = struct.unpack_from("<II", x, 48)
        assert image_size == stream_off + stream_size, self.stem
        listed = HEADER_BYTES + 12 * rc
        image = (listed + 12 + 31) & ~31
        table = (stream_off + stream_size + 3) & ~3
        out = bytearray(image + table + BANK_TABLE_BYTES)
        out[:HEADER_BYTES] = x[:HEADER_BYTES]
        out[HEADER_BYTES:listed] = x[ro:ro + 12 * rc]
        struct.pack_into("<III", out, listed, 1, *core[1])
        out[image:image + image_size] = x[image_off:image_off + image_size]
        flags = struct.unpack_from("<I", x, 12)[0] | FLAG_BANKS
        struct.pack_into("<4I", out, 8, len(out), flags, image, table + BANK_TABLE_BYTES)
        struct.pack_into("<II", out, 40, *level[1])
        struct.pack_into("<I", out, 48, HEADER_BYTES)
        struct.pack_into("<I", out, 60, listed)
        for i, (pair, data) in enumerate(self.relocs):
            digest = hashlib.sha1(data).digest()
            new = core[0][digest] | 1 << SLOT_SHIFT if digest in core[0] else level[0][digest]
            struct.pack_into("<I", out, HEADER_BYTES + 12 * i + 4, new)
            control = struct.unpack_from("<H", out, image + pair)[0]
            struct.pack_into("<HH", out, image + pair, (control & ~0x7F) | (new >> 16), new & 0xFFFF)
        struct.pack_into("<I", out, 32, control_id(out[image:image + table + BANK_TABLE_BYTES]))
        return bytes(out)


def make_bank(samples):
    """An AFB holding samples (digest -> bytes) 32-byte aligned; returns (file, offsets, id)."""
    payload, offsets = bytearray(), {}
    for digest, data in samples.items():
        payload += bytes(-len(payload) % 32)
        offsets[digest] = len(payload)
        payload += data
    payload += bytes(-len(payload) % 32)
    d = hashlib.sha1(payload).digest()
    bank_id = (struct.unpack_from("<I", d, 0)[0] | 1, struct.unpack_from("<I", d, 4)[0])
    header = b"AFB\0" + struct.pack("<IIIIIII", 1, bank_id[0], bank_id[1], 32, len(payload), 32 + len(payload), 0)
    return header + bytes(payload), offsets, bank_id


def write_pack(path, bank_file, entries):
    """entries: (stem, bank index, afx bytes)."""
    out = bytearray(MAGIC + struct.pack("<4I", 3, len(entries), 0, 0) + bytes(ENTRY.size * len(entries)))
    out += bytes(-len(out) % 32)
    struct.pack_into("<II", out, 12, len(out), len(bank_file))
    out += bank_file
    for i, (stem, which, afx) in enumerate(entries):
        out += bytes(-len(out) % 32)
        ENTRY.pack_into(out, 20 + i * ENTRY.size, stem.encode(), which, len(out), len(afx), 0)
        out += afx
    path.write_bytes(bytes(out))
    return len(out)


adpcm_cache = {}


def fit(ranked, core_samples, budget, music, forced_out=()):
    """The assets of ranked (in load order) that go in a level pack, and those left out.

    An asset costs its flow and the samples it adds to the level bank, which are those the core
    bank does not hold. The level's music and sounds go first while they fit, and
    leave free what the largest other song needs beyond the flows of the level's music, which
    the runtime lets go for it: a sound left out loads from its own files when it plays, but a
    song left out without room is silent. Of the other songs, the set packed is the one that lets the most of them play: a song left out
    plays when the room it needs on its own (its files, which the runtime loads when it plays)
    is free beside the pack; ties go to the larger set packed, which loads nothing mid-scene.
    forced_out (chosen to load from disk) are never packed, but their songs count among those
    the room is kept for."""
    core = set(core_samples)

    def cost(a, have):
        return len(a.afx) + sum(len(v) for k, v in a.samples().items() if k not in have and k not in core)

    def alone(a):
        return len(a.afx) + sum(-len(v) % 32 + len(v) for v in a.samples().values())

    def place(assets, have, used, limit):
        taken, out = [], []
        for a in assets:
            c = cost(a, have)
            if used + c <= limit:
                taken.append(a)
                used += c
                have = have | (set(a.samples()) - core)
            else:
                out.append(a)
        return taken, out, have, used

    later = [a for a in ranked if a.stem.startswith("seq") and a.stem not in music]
    level_music = sum(len(a.afx) for a in ranked if a.stem in music)
    forced_songs = [a for a in forced_out if a.stem.startswith("seq")]
    reserve = max(0, max((alone(a) for a in later + forced_songs), default=0) - level_music)
    first, first_out, have, used = place([a for a in ranked if a not in later], set(), 0, budget - reserve)
    best = None
    for bits in range(1 << len(later)):
        packed, out, _, total = place([a for i, a in enumerate(later) if bits >> i & 1], have, used, budget)
        if out:
            continue
        rest = [a for i, a in enumerate(later) if not bits >> i & 1] + forced_songs
        idle = level_music + sum(len(a.afx) for a in packed)
        room = budget - total + idle >= max((alone(a) for a in rest), default=0)
        score = (len(packed) + (len(rest) if room else 0), len(packed))
        if best is None or score > best[0]:
            best = (score, packed, rest, room)
    _, packed, rest, room = best
    for a in first_out:
        print(f"  warning: {a.stem} does not fit", file=sys.stderr)
    if rest and not room:
        print(f"  note: no room beside the whole pack for {' '.join(a.stem for a in rest)} (the runtime lets idle songs go for them)", file=sys.stderr)
    rest = [a for a in rest if a not in forced_songs]
    return first + packed, rest + first_out + list(forced_out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("assets", type=Path, help="sm64_scene_assets.py --json output")
    ap.add_argument("--stage", type=Path, default=ROOT / "build/afx/stage")
    # afx_mem_stats on hardware: used + free with nothing loaded but the firmware's own state.
    ap.add_argument("--arena-kb", type=int, default=2023, help="AICA RAM arena (default %(default)s)")
    ap.add_argument("--reverb-kb", type=int, default=16, help="reverb ring, 0 without (default %(default)s)")
    ap.add_argument("--margin-kb", type=int, default=32, help="kept free (default %(default)s)")
    ap.add_argument("--choices", type=Path, help="moves, additions, disk-only assets and ADPCM samples (sm64_sound_choices.json)")
    args = ap.parse_args()
    stage = args.stage / "afx"
    lists = json.loads(args.assets.read_text())
    choices = json.loads(args.choices.read_text()) if args.choices else {}
    adpcm = set(choices.get("adpcm", []))
    disk = set(choices.get("disk", []))
    for stem, levels in list(choices.get("move", {}).items()) + list(choices.get("add", {}).items()):
        if stem in lists["global"]:
            lists["global"].remove(stem)
        for lvl in levels:
            if stem not in lists["levels"].setdefault(lvl, []):
                lists["levels"][lvl].append(stem)
    lists["global"] = [s for s in lists["global"] if s not in disk]

    def load(stems):
        out = []
        for s in stems:
            if (stage / f"{s}.afx").exists() and (stage / f"{s}.afb").exists():
                out.append(Asset(stage, s, adpcm))
        return out

    core = load(lists["global"])
    core_samples = {}
    for a in core:
        core_samples.update(a.samples())
    core_bank, core_offsets, core_id = make_bank(core_samples)
    size = write_pack(stage / "scene_core.smp", core_bank,
                      [(a.stem, 0, a.bound(core_offsets, core_id)) for a in core])
    core_cost = len(core_bank) + sum(len(a.afx) for a in core)
    budget = (args.arena_kb - args.reverb_kb - args.margin_kb) * 1024 - core_cost
    print(f"scene_core.smp: {len(core)} assets, bank {len(core_bank) // 1024} KB, pack {size // 1024} KB; "
          f"{budget // 1024} KB left for a level")
    numbers = level_numbers()
    report = [f"core {len(core)} assets bank {len(core_bank)}"]
    by_number = {}
    for lvl, stems in sorted(lists["levels"].items()):
        if lvl in numbers and stems:
            entry = by_number.setdefault(numbers[lvl], (lvl, set(), set()))
            entry[1].update(stems)
            entry[2].update(lists.get("music", {}).get(lvl, []))
    for number, (lvl, stems, music) in sorted(by_number.items()):
        def rank(a):
            return (0 if a.stem in music else 1 if a.stem.startswith("sfx_") else 2, len(a.afx), a.stem)
        assets = load(stems)
        own, left_out = fit(sorted((a for a in assets if a.stem not in disk), key=rank), core_samples, budget,
                            music - disk, [a for a in assets if a.stem in disk])
        if not own:
            (stage / f"scene_{number:02d}.smp").unlink(missing_ok=True)
            print(f"scene_{number:02d}.smp {lvl:17s} none: everything is on disk {' '.join(a.stem for a in left_out)}")
            report.append(f"{number:02d} {lvl} 0 assets left out {' '.join(a.stem for a in left_out)}")
            continue
        samples = {}
        for a in own:
            samples.update((k, v) for k, v in a.samples().items() if k not in core_samples)
        assert samples, f"{lvl}: every sample is in the core bank, so the level has no bank"
        bank, offsets, bank_id = make_bank(samples)

        def entry(a):
            s = set(a.samples())
            if s <= set(core_samples):
                return (a.stem, 1, a.bound(core_offsets, core_id))
            if s & set(core_samples):
                return (a.stem, 2, a.bound_both((offsets, bank_id), (core_offsets, core_id)))
            return (a.stem, 0, a.bound(offsets, bank_id))
        entries = [entry(a) for a in own]
        on_core = [e for e in entries if e[1] == 1]
        both = [e for e in entries if e[1] == 2]
        # Songs load last: the room one leaves when it lets go joins the free end of AICA RAM.
        entries.sort(key=lambda e: e[0].startswith("seq"))
        size = write_pack(stage / f"scene_{number:02d}.smp", bank, entries)
        flows = sum(struct.unpack_from("<I", e[2], 20)[0] for e in entries)
        out = f" left out {' '.join(a.stem for a in left_out)}" if left_out else ""
        print(f"scene_{number:02d}.smp {lvl:17s} {len(own):3d} assets ({len(on_core)} on core, {len(both)} on both), "
              f"bank {len(bank) // 1024:4d} KB, flows {flows // 1024:4d} KB{out}")
        report.append(f"{number:02d} {lvl} {len(own)} assets bank {len(bank)} flows {flows}{out}")
    (stage / "scenes.txt").write_text("\n".join(report) + "\n")


if __name__ == "__main__":
    main()
