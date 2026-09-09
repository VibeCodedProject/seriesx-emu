#!/usr/bin/env python3
"""Dump the kernel thunk ordinals of the vendored LithiumX XBE and map
them to names via the xboxdevwiki export table. This gives the REAL
import profile of a real homebrew XBE to drive the HLE subset choice.

Format knowledge used (all previously verified against Cxbx/pyxbe/wiki):
  - header 0x158: kernel_thunk_dir_addr, XOR-encoded
    (retail 0x5B6D40B6 / debug 0xEFB1F152)
  - entries: u32; entry & 0x80000000 -> import, ordinal = entry & 0x7FFFFFFF
    (verified on this XBE in C++: 103 thunks starting ordinal 52)
  - section virtual_addr fields are ABSOLUTE VAs (loader bugfix note)
"""
import struct
import sys

XBE = "/home/z/my-project/seriesx-emu/seriesx-emu/tests/lithiumx.xbe"
TSV = "/home/z/my-project/scripts/research/kernel_exports.tsv"
XOR = {0xA8FC57AB: "retail", 0x94859D4B: "debug"}
KT_XOR = {0x5B6D40B6: "retail", 0xEFB1F152: "debug"}

data = open(XBE, "rb").read()
assert data[0:4] == b"XBEH", "not an XBE"
base = struct.unpack_from("<I", data, 0x104)[0]
nsec = struct.unpack_from("<I", data, 0x11C)[0]
sec_hdr = struct.unpack_from("<I", data, 0x120)[0]
kt_enc = struct.unpack_from("<I", data, 0x158)[0]
ep_enc = struct.unpack_from("<I", data, 0x128)[0]

# Section table for VA->file-offset resolution. section_headers_addr is
# a VA inside the header block (file offset = va - base for the header
# region; verified: LithiumX sec_hdr VA 0x10348 -> file 0x348).
secs = []
for i in range(nsec):
    o = sec_hdr - base + i * 0x38
    flags, va, vsz, raw, rsz = struct.unpack_from("<IIIII", data, o)
    secs.append((va, vsz, raw, rsz))
secs.sort()

def va_to_off(va):
    if va >= base and va < base + 0x1000:  # header region
        return va - base
    for sva, vsz, raw, rsz in secs:
        if sva <= va < sva + max(vsz, rsz):
            d = va - sva
            if d < rsz:
                return raw + d
            return None  # BSS: no file bytes
    return None

# Decode type + thunk dir.
kt_type = None
kt_va = None
for key, kind in KT_XOR.items():
    cand = kt_enc ^ key
    off = va_to_off(cand)
    if off is not None:
        kt_type, kt_va = kind, cand
        break
ep_type = None
ep_va = None
for key, kind in XOR.items():
    cand = ep_enc ^ key
    if base <= cand < base + (1 << 24):
        ep_type, ep_va = kind, cand
        break
print(f"thunk dir VA 0x{kt_va:08X} ({kt_type}), entry VA 0x{ep_va:08X} ({ep_type})")

ordinals = []
off = va_to_off(kt_va)
while off is not None and off + 4 <= len(data):
    (e,) = struct.unpack_from("<I", data, off)
    if e == 0:
        break
    if e & 0x80000000:
        ordinals.append(e & 0x7FFFFFFF)
    off += 4
print(f"thunk entries: {len(ordinals)}")

names = {}
convs = {}
for line in open(TSV):
    o, n, c = line.rstrip("\n").split("\t")
    names[int(o)] = n
    convs[int(o)] = c

from collections import Counter
print("\n# ordinal name convention")
unknown = []
for o in ordinals:
    if o in names:
        print(f"{o}\t{names[o]}\t{convs[o]}")
    else:
        unknown.append(o)
        print(f"{o}\t<UNKNOWN>\t?")
print("\nunknown ordinals:", unknown)
cats = Counter(names.get(o, "?").split("I")[0] if False else names.get(o, "?")[0:2] for o in ordinals)
print("import histogram:", dict(cats))
