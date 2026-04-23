#!/usr/bin/env python3
"""Build a kanji_<size>_<style>.epkf overlay file for Shrike Reader.

EPKF (EPD Packed Kanji Font) container layout:

  offset size  field
  ---------------------------------------------------------------
  0x00   4     magic           'EPKF'
  0x04   2     version         uint16 LE (= 1)
  0x06   2     flags           uint16 LE (bit0=2bit, bit1=compressed)
  0x08   2     fontSize        uint16 LE (px)
  0x0A   2     ascent          int16 LE (px)
  0x0C   2     descent         int16 LE (px)
  0x0E   2     advanceY        uint16 LE (px, line height)
  0x10   4     glyphCount      uint32 LE
  0x14   2     tileCount       uint16 LE  (=328 for U+4E00..U+9FFF / 64)
  0x16   2     tileFirstCp     uint16 LE  (always 0x4E00 >> 6 = 0x138, but kept
                                           explicit for future extensions to
                                           CJK Extension A)
  0x18   4     tileIndexOffset uint32 LE  (file offset to tile-index array)
  0x1C   4     glyphTableOffset uint32 LE (file offset to EpdGlyph-like array)
  0x20   4     bitmapPoolOffset uint32 LE
  0x24   4     bitmapPoolSize   uint32 LE
  0x28   24    reserved         zeroed

Total header = 64 bytes.

Tile-index array: tileCount entries, each 10 bytes packed:
  uint64 presence     LSB = first codepoint of the tile
  uint16 firstGlyphId index into glyph-table of first glyph in this tile
(Note: packed struct is 10 bytes; we pad to 12 for 4-byte alignment in RAM
 but keep the file compact at 10 bytes each.)

Glyph table: glyphCount entries, each 16 bytes — same EpdGlyph layout used
in flash fonts (width, height, advanceX fp4, left, top, dataLength,
dataOffset). dataOffset is absolute file offset into BITMAP POOL.

Bitmap pool: concatenated per-glyph raw-DEFLATE streams. Each glyph's
compressed block length is `dataLength`; its decompressed form is a
byte-aligned 2-bit bitmap (same byte-aligned convention fontconvert.py
uses for --2bit compressed fonts, i.e. each row padded to a byte).

Italic/BoldItalic fall back to regular/bold (same Phase 1 policy).
"""

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path

import freetype

EPKF_MAGIC = b"EPKF"
EPKF_VERSION = 1
HEADER_SIZE = 64
TILE_ENTRY_SIZE = 10   # uint64 + uint16 (packed in the file)
GLYPH_ENTRY_SIZE = 14  # matches EpdGlyph packed layout (u8+u8+u16+i16+i16+u16+u32)

CJK_START = 0x4E00
CJK_END = 0x9FFF  # inclusive last codepoint of Unified Ideographs
TILE_BITS = 64
TILES = (CJK_END - CJK_START + 1 + TILE_BITS - 1) // TILE_BITS  # 328


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def load_codepoints(path: Path) -> list[int]:
    cps = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # One kanji character per line.
        cp = ord(line[0])
        if not (CJK_START <= cp <= CJK_END):
            raise SystemExit(f"codepoint U+{cp:04X} outside {CJK_START:X}..{CJK_END:X}")
        cps.append(cp)
    # Sort & dedupe.
    return sorted(set(cps))


def rasterize_2bit(face: freetype.Face, cp: int):
    """Render `cp` at the face's current size, returning (packed2bit, width,
    height, advanceX_fp4, left, top).  Byte-aligns each row (same convention
    fontconvert.py uses when `--2bit` + compression is active)."""
    gidx = face.get_char_index(cp)
    if gidx == 0:
        return None
    face.load_glyph(gidx, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_FORCE_AUTOHINT)
    bm = face.glyph.bitmap
    w, h = bm.width, bm.rows

    # 4-bit greyscale intermediate (2 px / byte) — mirrors fontconvert.py.
    pixels4g = []
    px = 0
    for i, v in enumerate(bm.buffer):
        x = i % w if w else 0
        if x % 2 == 0:
            px = (v >> 4) & 0x0F
        else:
            px |= (v & 0xF0)
            pixels4g.append(px)
            px = 0
        if w and x == w - 1 and w % 2 == 1:
            pixels4g.append(px)
            px = 0

    # Convert to 2-bit with ROW-byte alignment (each row padded to full byte).
    row_bytes = (w + 3) // 4
    pitch4g = (w // 2) + (w % 2)
    out = bytearray(row_bytes * h)
    for y in range(h):
        for x in range(w):
            src = pixels4g[y * pitch4g + (x // 2)] if pixels4g else 0
            nibble = (src >> ((x % 2) * 4)) & 0x0F
            if nibble >= 12:
                q = 3
            elif nibble >= 8:
                q = 2
            elif nibble >= 4:
                q = 1
            else:
                q = 0
            bit_pos = (3 - (x % 4)) * 2
            out[y * row_bytes + (x // 4)] |= q << bit_pos

    # advanceX in 12.4 fp4 (linearHoriAdvance is 16.16 -> shift right 12).
    adv_fp4 = (face.glyph.linearHoriAdvance + (1 << 11)) >> 12
    return (
        bytes(out),
        w,
        h,
        adv_fp4 & 0xFFFF,
        face.glyph.bitmap_left,
        face.glyph.bitmap_top,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="Input TTF/OTF (NotoSansCJKjp)")
    ap.add_argument("--size", type=int, required=True, help="Font size in px")
    ap.add_argument(
        "--codepoints",
        required=True,
        type=Path,
        help="File with one kanji character per line",
    )
    ap.add_argument("--out", required=True, type=Path, help="Output .epkf path")
    ap.add_argument(
        "--dpi", type=int, default=150, help="Target DPI (matches fontconvert.py)"
    )
    args = ap.parse_args()

    codepoints = load_codepoints(args.codepoints)
    log(f"[epkf] {len(codepoints)} codepoints from {args.codepoints}")

    face = freetype.Face(str(args.font))
    face.set_char_size(args.size << 6, args.size << 6, args.dpi, args.dpi)

    # Rasterise each glyph.
    glyphs = []  # (cp, packed_raw, w, h, adv_fp4, left, top)
    for cp in codepoints:
        res = rasterize_2bit(face, cp)
        if res is None:
            log(f"[epkf] missing glyph U+{cp:04X}, skipping")
            continue
        glyphs.append((cp,) + res)

    log(f"[epkf] rasterised {len(glyphs)} glyphs")

    # Build tile index (328 tiles of 64 codepoints covering U+4E00..U+9FFF).
    # presence[i] bit j => codepoint (CJK_START + i*64 + j) is present.
    presence = [0] * TILES
    # glyphs are already in ascending codepoint order because we sort the
    # codepoint list at load time.
    first_glyph_id = [0] * TILES
    have_first = [False] * TILES
    for idx, (cp, *_rest) in enumerate(glyphs):
        off = cp - CJK_START
        t, b = off >> 6, off & 63
        presence[t] |= 1 << b
        if not have_first[t]:
            first_glyph_id[t] = idx
            have_first[t] = True

    # Compress each glyph bitmap individually with raw DEFLATE.
    compressed_blobs = []
    for (_cp, raw, *_rest) in glyphs:
        comp = zlib.compressobj(level=9, wbits=-15)
        blob = comp.compress(raw) + comp.flush()
        compressed_blobs.append(blob)

    # Compute layout offsets.
    tile_index_off = HEADER_SIZE
    glyph_table_off = tile_index_off + TILES * TILE_ENTRY_SIZE
    bitmap_pool_off = glyph_table_off + len(glyphs) * GLYPH_ENTRY_SIZE
    bitmap_pool = bytearray()
    glyph_records = []
    for i, ((cp, _raw, w, h, adv, left, top), comp) in enumerate(
        zip(glyphs, compressed_blobs)
    ):
        data_offset = bitmap_pool_off + len(bitmap_pool)
        data_length = len(comp)
        bitmap_pool.extend(comp)
        # EpdGlyph: width u8, height u8, advanceX u16, left i16, top i16,
        # dataLength u16, dataOffset u32
        glyph_records.append(
            struct.pack(
                "<BBHhhHI",
                min(w, 255),
                min(h, 255),
                adv & 0xFFFF,
                left,
                top,
                data_length & 0xFFFF,
                data_offset & 0xFFFFFFFF,
            )
        )

    # Derive ascent/descent/advanceY from the face metrics.
    ascent_px = face.size.ascender >> 6
    descent_px = face.size.descender >> 6  # negative
    advance_y = face.size.height >> 6

    # --- Write the file. ---
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("wb") as f:
        # Header
        flags = 0b01  # bit 0: 2-bit
        flags |= 0b10  # bit 1: per-glyph DEFLATE compressed
        f.write(EPKF_MAGIC)
        f.write(struct.pack("<H", EPKF_VERSION))
        f.write(struct.pack("<H", flags))
        f.write(struct.pack("<H", args.size))
        f.write(struct.pack("<hh", ascent_px, descent_px))
        f.write(struct.pack("<H", advance_y))
        f.write(struct.pack("<I", len(glyphs)))
        f.write(struct.pack("<H", TILES))
        f.write(struct.pack("<H", CJK_START >> 6))
        f.write(struct.pack("<I", tile_index_off))
        f.write(struct.pack("<I", glyph_table_off))
        f.write(struct.pack("<I", bitmap_pool_off))
        f.write(struct.pack("<I", len(bitmap_pool)))
        f.write(b"\x00" * (HEADER_SIZE - f.tell()))

        # Tile index
        assert f.tell() == tile_index_off
        for t in range(TILES):
            f.write(struct.pack("<QH", presence[t], first_glyph_id[t]))

        # Glyph records
        assert f.tell() == glyph_table_off
        for rec in glyph_records:
            f.write(rec)

        # Bitmap pool
        assert f.tell() == bitmap_pool_off
        f.write(bitmap_pool)

    size_b = args.out.stat().st_size
    log(f"[epkf] wrote {args.out}  {size_b} bytes  "
        f"(tiles={TILES}, glyphs={len(glyphs)}, pool={len(bitmap_pool)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
