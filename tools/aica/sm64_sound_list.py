#!/usr/bin/env python3
"""Every sound the game can ask for, as the soundBits tools/aica/recorder records (hex, comma-separated).

    tools/aica/sm64_sound_list.py > build/audio_rec/sounds.txt

A bank's sounds are the entries of its table in sound/sequences/00_sound_player.s (the US ones, up to
sNumSoundsPerBank), not only the named ones of include/audio_defines.h: the game also picks them as
name + (n << 16), such as the next red coin's note or Mario's voices. An entry without a name takes
the flags of the name below it. Banks 8 and 9 run the tables of 3 and 5; only their named sounds count.
"""
import bisect
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BANK_SIZES = [0x70, 0x30, 0x40, 0x80, 0x20, 0x80, 0x20, 0x40]   # src/audio/external.c, non-JP


def main():
    number = r"(0x[0-9A-Fa-f]+|\d+)"
    named = {}
    for m in re.finditer(rf"#define\s+SOUND_\w+\s+SOUND_ARG_LOAD\(\s*{number},\s*{number},\s*{number},\s*{number},\s*{number}",
                         (ROOT / "include/audio_defines.h").read_text()):
        bank, flags, sound, priority, flags2 = (int(x, 0) for x in m.groups())
        named.setdefault(bank, {})[sound] = (flags, priority, flags2)
    lines = [l.split("/*")[0].strip() for l in (ROOT / "sound/sequences/00_sound_player.s").read_text().splitlines()]
    labels = {l[:-1]: i for i, l in enumerate(lines) if l.endswith(":")}
    channels = dict(m.groups() for m in (re.fullmatch(r"seq_startchannel (\d+), (\.\w+)", l) for l in lines) if m)

    def table(channel):
        at = next(i for i in range(labels[channel], len(lines)) if lines[i].startswith("chan_setdyntable"))
        at, skipping, count = labels[lines[at].split()[1]] + 1, False, 0
        for l in lines[at:]:
            if l.startswith((".ifdef", ".ifndef", ".else", ".endif")):
                skipping = l in (".ifdef VERSION_JP", ".ifdef VERSION_EU") or (l == ".else" and not skipping)
            elif l and not skipping:
                if not l.startswith("sound_ref"):
                    break
                count += 1
        return count

    bits = []
    for bank, size in enumerate(BANK_SIZES):
        ids = sorted(named[bank])
        for sound in range(min(table(channels[str(bank)]), size)):
            flags, priority, flags2 = named[bank][ids[max(bisect.bisect_right(ids, sound) - 1, 0)]]
            bits.append(bank << 28 | flags << 24 | sound << 16 | priority << 8 | flags2 << 4 | 1)
    for bank in (8, 9):
        for sound, (flags, priority, flags2) in sorted(named[bank].items()):
            bits.append(bank << 28 | flags << 24 | sound << 16 | priority << 8 | flags2 << 4 | 1)
    print(",".join(f"{b:08x}" for b in bits))


if __name__ == "__main__":
    main()
