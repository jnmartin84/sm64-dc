#!/usr/bin/env python3
"""
SM64 ALBankFile/AudioBank parser. Parses the built sound_data.ctl + sound_data.tbl
(VERSION_US, sm64-port host build => LITTLE-endian, 32-bit) and enumerates/dedups
every referenced VADPCM sample into transcode-compatible Sample objects (same
interface as the MK64/OoT parsers so transcode.py/yamaha_adpcm.py can be reused).

Both files are ALSeqFile wrappers:
  header { s16 revision; s16 seqCount } then seqArray[seqCount]{ u32 off; u32 len }
Unlike MK64 (one shared tbl blob), SM64 has a PER-BANK .tbl region: each bank's
samples have sampleAddr relative to gAlTbl->seqArray[bankId].offset. So the unique
key = absolute tbl offset = tblRegionOff(bank) + sampleAddr (also the runtime key,
since patched sampleAddr = gSoundDataRaw + tblRegionOff + sampleAddr_rel).

ctl bank i: region at ctl.seqArray[i].off begins with [u32 numInstruments][u32 numDrums];
  AudioBank proper at +0x10; internal offsets relative to that bankBase.
"""

import struct

FRAME = {0: (16, 9)}   # VADPCM: 16 samples / 9 bytes


def le16(b, o):
    return struct.unpack_from("<h", b, o)[0]

def le32(b, o):
    return struct.unpack_from("<I", b, o)[0]

def lef32(b, o):
    return struct.unpack_from("<f", b, o)[0]


class Sample:
    __slots__ = ("bank", "addr", "size", "codec", "has_loop", "loop_start",
                 "loop_end", "loop_count", "nsamples", "order", "npredictors",
                 "book", "data", "banks", "tunings")

    def __init__(self, addr, size, codec, loop, book, data):
        self.bank = 0          # absolute-tbl-offset key space
        self.addr = addr       # absolute offset into the .tbl file = runtime key
        self.size = size
        self.codec = codec
        if loop is None:
            self.has_loop, self.loop_start, self.loop_end, self.loop_count = False, 0, 0, 0
        else:
            self.loop_start, self.loop_end, self.loop_count = loop
            self.has_loop = self.loop_count != 0
        smp_per, byte_per = FRAME[codec]
        self.nsamples = (size // byte_per) * smp_per
        self.order, self.npredictors, self.book = book
        self.data = data
        self.banks = set()
        self.tunings = set()


def _read_alseqfile(buf):
    """Return (revision, seqCount, [(off,len)...])."""
    rev = le16(buf, 0)
    n = le16(buf, 2)
    entries = []
    o = 4
    for _ in range(n):
        off = le32(buf, o)
        ln = le32(buf, o + 4)
        entries.append((off, ln))
        o += 8
    return rev, n, entries


def _read_book(ab, off):
    order = le32(ab, off)
    npred = le32(ab, off + 4)
    n = 8 * order * npred
    coeffs = struct.unpack_from(f"<{n}h", ab, off + 8)
    return order, npred, coeffs


def _read_loop(ab, off):
    start, end, count, _pad = struct.unpack_from("<IIII", ab, off)
    return (start, end, count)


def parse_all(ctl_path, tbl_path, with_data=True):
    with open(ctl_path, "rb") as f:
        ab = f.read()
    with open(tbl_path, "rb") as f:
        tbl_bytes = f.read()
    tbl = tbl_bytes if with_data else None
    tbl_for_regions = tbl_bytes

    _, _, ctl_entries = _read_alseqfile(ab)
    _, _, tbl_entries = _read_alseqfile(tbl_for_regions)

    samples = {}   # abs_tbl_offset -> Sample

    def read_sound(bank_base, sound_off, bank_idx, tuning, tbl_region_off):
        if sound_off == 0:
            return
        # AudioBankSound{u32 sample(rel bankBase); f32 tuning}
        samp_off = le32(ab, bank_base + sound_off + 0)
        if samp_off == 0:
            return
        sh = bank_base + samp_off
        # AudioBankSample: u8 unused, u8 loaded, pad2, u32 sampleAddr, u32 loop, u32 book, u32 sampleSize
        sample_addr = le32(ab, sh + 4)
        loop_off = le32(ab, sh + 8)
        book_off = le32(ab, sh + 12)
        sample_size = le32(ab, sh + 16)
        key = tbl_region_off + sample_addr   # absolute tbl offset
        if key in samples:
            samples[key].banks.add(bank_idx)
            samples[key].tunings.add(tuning)
            return
        book = _read_book(ab, bank_base + book_off)
        loop = _read_loop(ab, bank_base + loop_off) if loop_off else None
        data = None
        if tbl is not None:
            data = tbl[key: key + sample_size]
        s = Sample(key, sample_size, 0, loop, book, data)
        s.banks.add(bank_idx)
        s.tunings.add(tuning)
        samples[key] = s

    for i, (boff, blen) in enumerate(ctl_entries):
        if blen == 0:
            continue
        tbl_region_off = tbl_entries[i][0]
        num_inst = le32(ab, boff + 0)
        num_drums = le32(ab, boff + 4)
        bank_base = boff + 0x10
        # AudioBank: u32 drumsOff; u32 instPtr[num_inst]
        drums_off = le32(ab, bank_base + 0)
        for j in range(num_inst):
            ip = le32(ab, bank_base + 4 + 4 * j)
            if ip == 0:
                continue
            inst = bank_base + ip
            # Instrument: loaded,lo,hi,release (4); u32 env; 3x AudioBankSound{u32 sample, f32 tuning}
            for k in range(3):
                so = inst + 8 + k * 8
                samp = le32(ab, so)
                tun = lef32(ab, so + 4)
                if samp != 0 and tun != 0.0:
                    read_sound(bank_base, so - bank_base, i, tun, tbl_region_off)
        if drums_off:
            for d in range(num_drums):
                dp = le32(ab, bank_base + drums_off + 4 * d)
                if dp == 0:
                    continue
                drum = bank_base + dp
                # Drum: u8 release,pan,loaded,pad; AudioBankSound sound (off4); u32 env
                so = drum + 4
                samp = le32(ab, so)
                tun = lef32(ab, so + 4)
                if samp != 0 and tun != 0.0:
                    read_sound(bank_base, so - bank_base, i, tun, tbl_region_off)

    return 0, samples


if __name__ == "__main__":
    import sys
    _, samples = parse_all(sys.argv[1], sys.argv[2])
    print(f"unique samples={len(samples)}")
