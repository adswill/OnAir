#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Rebuild the OFL Chinese subsets from the original Sans2.004 OTFs (fontTools required)."""
import argparse
import hashlib
from pathlib import Path
import zlib

from fontTools import subset
from fontTools.pens.recordingPen import RecordingPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_HASHES = {
    "Regular": "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b",
    "Bold": "b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a",
}


def gb2312_characters():
    result = set()
    for lead in range(0xA1, 0xFF):
        for trail in range(0xA1, 0xFF):
            try:
                result.add(ord(bytes((lead, trail)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    if len(result) != 7445:
        raise ValueError(f"Expected 7445 GB2312 characters, got {len(result)}")
    return result


def catalog_characters():
    result = set()
    for line in (ROOT / "app/i18n/zh.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or "\t" not in line:
            continue
        result.update(ord(c) for c in line.split("\t", 1)[1] if ord(c) >= 0x20)
    return result


def rename_subset(font, weight):
    family = "OnAir CJK SC"
    full_name = f"{family} {weight}"
    postscript = f"OnAirCJKSC-{weight}"
    replacements = {
        1: family, 2: weight, 3: f"2.004;OnAir;{postscript};GB2312",
        4: full_name, 5: "Version 2.004; OnAir GB2312 subset", 6: postscript,
        10: "Modified for OnAir: GB2312 and Chinese interface characters. Original glyph outlines retained.",
        16: family, 17: weight,
    }
    for record in list(font["name"].names):
        if record.nameID in replacements:
            font["name"].setName(replacements[record.nameID], record.nameID, record.platformID, record.platEncID, record.langID)
    for name_id, text in replacements.items():
        for platform, encoding, language in ((3, 1, 0x409), (1, 0, 0)):
            font["name"].setName(text, name_id, platform, encoding, language)
    cff = font["CFF "].cff
    cff.fontNames[0] = postscript
    cff.topDictIndex[0].FamilyName = family
    cff.topDictIndex[0].FullName = full_name


def verify(original, output, characters):
    font = TTFont(output)
    old_map, new_map = original.getBestCmap(), font.getBestCmap()
    missing = characters - new_map.keys()
    if missing:
        raise ValueError(f"Missing requested characters: {sorted(missing)}")
    # Subsetting must not alter the line box, advances or retained glyph designs.
    for key in ("ascent", "descent", "lineGap"):
        if getattr(font["hhea"], key) != getattr(original["hhea"], key):
            raise ValueError(f"Line metric changed: {key}")
    if font["head"].unitsPerEm != original["head"].unitsPerEm:
        raise ValueError("Units per em changed")
    old_glyphs, new_glyphs = original.getGlyphSet(), font.getGlyphSet()
    for cp in sorted(characters):
        old_name, new_name = old_map[cp], new_map[cp]
        if original["hmtx"][old_name] != font["hmtx"][new_name]:
            raise ValueError(f"Glyph metrics changed: U+{cp:04X}")
        before, after = RecordingPen(), RecordingPen()
        old_glyphs[old_name].draw(before)
        new_glyphs[new_name].draw(after)
        if before.value != after.value:
            raise ValueError(f"Glyph outline changed: U+{cp:04X}")
    if not any(n.nameID == 0 for n in font["name"].names):
        raise ValueError("Missing copyright notice")
    if not any(n.nameID == 13 for n in font["name"].names):
        raise ValueError("Missing OFL notice")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream_dir", type=Path, help="original NotoSansCJKsc-{Regular,Bold}.otf directory")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "third_party/fonts/noto-cjk")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    gb = gb2312_characters()
    wanted = gb | catalog_characters() | set(range(0x20, 0x7F)) | {0xA0}
    total, compressed = 0, 0
    for weight, expected_hash in ORIGINAL_HASHES.items():
        name = f"NotoSansCJKsc-{weight}.otf"
        source, output = args.upstream_dir / name, args.output_dir / name
        if source.resolve() == output.resolve():
            raise ValueError("Keep originals outside the output directory")
        if hashlib.sha256(source.read_bytes()).hexdigest() != expected_hash:
            raise ValueError(f"Wrong upstream version: {source}")
        original = TTFont(source, recalcTimestamp=False)
        cmap = original.getBestCmap()
        if gb - cmap.keys():
            raise ValueError(f"Upstream font lacks GB2312 characters: {source}")
        kept = wanted & cmap.keys()
        print(f"{weight}: {len(kept)} characters; unavailable upstream symbols left to base/system fonts: "
              + ", ".join(f"U+{cp:04X}" for cp in sorted(wanted - cmap.keys())), flush=True)
        font = TTFont(source, recalcTimestamp=False)
        options = subset.Options()
        options.layout_features = ["*"]
        options.name_IDs = ["*"]  # retain copyright, license, version and provenance records
        options.name_languages = ["*"]
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(unicodes=sorted(kept))
        subsetter.subset(font)
        rename_subset(font, weight)
        font.save(output)
        verify(original, output, kept)
        raw = output.read_bytes()
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = compressor.compress(raw) + compressor.flush()
        total += len(raw)
        compressed += len(packed)
        print(f"{name}: {len(raw)} bytes, deflate {len(packed)} bytes, sha256 {hashlib.sha256(raw).hexdigest()}", flush=True)
    if total > 10 * 1024 * 1024:
        raise ValueError("CJK fonts exceed the 10 MiB budget")
    (args.output_dir / "gb2312.txt").write_text("".join(chr(cp) for cp in sorted(gb)), encoding="utf-8")
    print(f"Total: {total / 1048576:.2f} MiB fonts, {compressed / 1048576:.2f} MiB embedded resources")


if __name__ == "__main__":
    main()
