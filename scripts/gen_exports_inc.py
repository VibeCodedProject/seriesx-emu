#!/usr/bin/env python3
"""Generate src/xbox_krnl_exports.inc (ordinal table as X-macros) from the
nxdk import library definition (primary source, RE-derived from the real
xboxkrnl.exe) cross-checked against the xboxdevwiki export table.

Decoration grammar of xboxkrnl.exe.def (MS linker convention):
  Name@N            stdcall, N = callee-popped stack bytes -> arity N/4
  @Name@N           fastcall, N = TOTAL argument bytes (ECX/EDX first):
                    arity N/4, first 2 args in registers (stack pop = N-8
                    when N > 8, else 0)
  Name              cdecl (caller pops; DbgPrint varargs) or a DATA export
  ... DATA          exported data symbol (address, not code)

License: nxdk files are CC0-1.0 (def) / MIT (header set); xboxdevwiki text
is CC BY-SA. This generated table carries names + numeric ordinals only.
"""
import re
import os
import sys

# All paths are relative to THIS file so the script works from inside
# the repo on any machine: research inputs live in scripts/research/,
# the generated table overwrites src/xbox_krnl_exports.inc in place.
HERE = os.path.dirname(os.path.abspath(__file__))
DEF = os.path.join(HERE, "research", "nxdk_xboxkrnl.exe.def")
TSV = os.path.join(HERE, "research", "kernel_exports.tsv")
OUT = os.path.join(HERE, "..", "src", "xbox_krnl_exports.inc")

row_re = re.compile(
    r"^\s*(@?)([A-Za-z_][A-Za-z0-9_]*)(?:@(\d+))?\s+@\s*(\d+)\s+NONAME(?:\s+(DATA))?\s*$")

exports = {}  # ordinal -> (name, conv, bytes, data)
for line in open(DEF, encoding="utf-8"):
    m = row_re.match(line)
    if not m:
        continue
    fast, name, nbytes, ordinal, data = m.groups()
    if fast:
        conv = "FASTCALL"
        bytes_ = int(nbytes) if nbytes else 0
    elif nbytes is not None:
        conv = "STDCALL"
        bytes_ = int(nbytes)
    else:
        conv = "DATA" if data else "CDECL"
        bytes_ = 0
    o = int(ordinal)
    if o in exports:
        sys.exit(f"duplicate ordinal {o} in def")
    exports[o] = (name, conv, bytes_, bool(data))

wiki = {}
for line in open(TSV, encoding="utf-8"):
    o, n, c = line.rstrip("\n").split("\t")
    wiki[int(o)] = (n, c)

warn = 0
for o, (name, conv, bytes_, data) in sorted(exports.items()):
    if o not in wiki:
        print(f"W: ordinal {o} ({name}) missing from wiki table", file=sys.stderr)
        continue
    wname, wconv = wiki[o]
    if wname != name:
        print(f"W: name mismatch {o}: def={name} wiki={wname}", file=sys.stderr)
        warn += 1
    defconv = {"STDCALL": "stdcall", "FASTCALL": "fastcall", "CDECL": "cdecl",
               "DATA": "data"}.get(conv, "?")
    if wconv not in ("?", defconv) and not (wconv == "stdcall" and conv in ("CDECL",)):
        print(f"W: conv mismatch {o} {name}: def={defconv} wiki={wconv}", file=sys.stderr)
        warn += 1

with open(OUT, "w", encoding="utf-8") as f:
    f.write("// GENERATED FILE - do not edit by hand.\n")
    f.write("// Source: nxdk lib/xboxkrnl/xboxkrnl.exe.def (CC0-1.0, RE-derived\n")
    f.write("// from the real xboxkrnl.exe import library), cross-checked against\n")
    f.write("// xboxdevwiki.net/Kernel (CC BY-SA). Regenerate with\n")
    f.write("// scripts/gen_exports_inc.py after refreshing the research files.\n")
    f.write("//\n")
    f.write("// KRNL_EXPORT(ordinal, Name, conv, arg_bytes, is_data)\n")
    f.write("//   conv:     STDCALL | FASTCALL | CDECL | DATA\n")
    f.write("//   arg_bytes: stdcall = callee-popped stack bytes (arity/4);\n")
    f.write("//              fastcall = TOTAL arg bytes (first 8 via ECX/EDX);\n")
    f.write("//              CDECL/DATA = 0\n")
    for o, (name, conv, bytes_, data) in sorted(exports.items()):
        f.write(f"KRNL_EXPORT({o}, {name}, {conv}, {bytes_}, "
                f"{'true' if data else 'false'})\n")

n_std = sum(1 for v in exports.values() if v[1] == "STDCALL")
n_fast = sum(1 for v in exports.values() if v[1] == "FASTCALL")
n_cdecl = sum(1 for v in exports.values() if v[1] == "CDECL")
n_data = sum(1 for v in exports.values() if v[1] == "DATA")
print(f"exports: {len(exports)} (stdcall={n_std} fastcall={n_fast} "
      f"cdecl={n_cdecl} data={n_data}) warnings={warn}")
print("saved:", OUT)
