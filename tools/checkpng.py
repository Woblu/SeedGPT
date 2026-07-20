"""Validate a PNG written by tools/map.c: signature, every chunk CRC, and a
zlib stream that inflates to exactly height*(1 + width*3) bytes.

The encoder is hand-rolled (stored deflate blocks, no image library), so this
checks it produces a genuinely spec-valid file rather than merely a non-empty
one that happens to open in whatever viewer was handy.
"""
import struct, sys, zlib

path = sys.argv[1]
d = open(path, "rb").read()
if d[:8] != b"\x89PNG\r\n\x1a\n":
    sys.exit("bad PNG signature")
w, h = struct.unpack(">II", d[16:24])

i, idat, seen = 8, b"", []
while i < len(d):
    ln = struct.unpack(">I", d[i:i + 4])[0]
    typ = d[i + 4:i + 8]
    body = d[i + 8:i + 8 + ln]
    crc = struct.unpack(">I", d[i + 8 + ln:i + 12 + ln])[0]
    if zlib.crc32(typ + body) & 0xffffffff != crc:
        sys.exit(f"CRC mismatch in {typ.decode(errors='replace')} chunk")
    seen.append(typ.decode(errors="replace"))
    if typ == b"IDAT":
        idat += body
    i += 12 + ln

for required in ("IHDR", "IDAT", "IEND"):
    if required not in seen:
        sys.exit(f"missing {required} chunk")

raw = zlib.decompress(idat)
expect = h * (1 + w * 3)
if len(raw) != expect:
    sys.exit(f"inflated {len(raw)} bytes, expected {expect}")
print(f"{w}x{h}, {len(seen)} chunks, CRCs ok, zlib inflates to {len(raw)}B")
