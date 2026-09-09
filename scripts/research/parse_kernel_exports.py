#!/usr/bin/env python3
"""Extract the kernel export table (ordinal -> name) from the fetched
xboxdevwiki.net/Kernel page. Saves a clean TSV for the emulator's HLE work."""
import html
import re
import sys

SRC = "/home/z/my-project/scripts/research/xboxdevwiki_kernel.html"
OUT = "/home/z/my-project/scripts/research/kernel_exports.tsv"

with open(SRC, "r", encoding="utf-8", errors="replace") as f:
    page = f.read()

# The export list on xboxdevwiki.net/Kernel lives in wikitable rows:
# <tr><td>1</td><td>AvGetSavedDataAddress</td>... Find all table rows and
# keep those whose first cell is a decimal ordinal and second a C name.
rows = re.findall(r"<tr>(.*?)</tr>", page, re.S)
print(f"table rows found: {len(rows)}", file=sys.stderr)

# Export table rows: | Kernel/Name (link) | ordinal | convention | notes |
# Older wikitables also use plain "Name" links without the Kernel/ prefix.
exports = {}
conventions = {}
for row in rows:
    cells = re.findall(r"<t[dh][^>]*>(.*?)</t[dh]>", row, re.S)
    if len(cells) < 3:
        continue
    name = html.unescape(re.sub(r"<[^>]+>", "", cells[0])).strip()
    ordc = html.unescape(re.sub(r"<[^>]+>", "", cells[1])).strip()
    conv = html.unescape(re.sub(r"<[^>]+>", "", cells[2])).strip()
    name = re.sub(r"^Kernel/", "", name)
    if (re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
            and re.fullmatch(r"\d{1,3}", ordc)
            and conv in ("stdcall", "fastcall", "cdecl", "")):
        o = int(ordc)
        if o in exports and exports[o] != name:
            print(f"CONFLICT ordinal {o}: {exports[o]} vs {name}", file=sys.stderr)
        exports[o] = name
        conventions[o] = conv or "?"

print(f"exports parsed: {len(exports)}", file=sys.stderr)
if not exports:
    # Dump a raw sample so we can adapt the regex.
    sample = re.search(r"wikitable.*?</table>", page, re.S)
    print("NO EXPORTS PARSED; first table sample:", file=sys.stderr)
    print((sample.group(0)[:2000] if sample else "no wikitable") , file=sys.stderr)
    sys.exit(1)

with open(OUT, "w", encoding="utf-8") as f:
    for ordinal in sorted(exports):
        f.write(f"{ordinal}\t{exports[ordinal]}\t{conventions[ordinal]}\n")

# Spot-check the ordinals our loader worklog cares about.
for probe in (1, 52, 100, 154, 344, 357, 358, 359, 366):
    print(probe, "->", exports.get(probe), f"({conventions.get(probe)})")
print("range:", min(exports), "..", max(exports), "count:", len(exports))
conv_counts = {}
for o, c in conventions.items():
    conv_counts[c] = conv_counts.get(c, 0) + 1
print("conventions:", conv_counts)
print("saved:", OUT)
