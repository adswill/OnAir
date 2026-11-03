# Noto Sans CJK SC subsets for OnAir

Modified subsets of the static OpenType fonts from Sans2.004 (font version 2.004).
Copyright 2014-2021 Adobe (http://www.adobe.com/), retained in the font name table.
Licensed under SIL Open Font License 1.1; see LICENSE and ../../../LICENSES/OFL-1.1.txt.
The modified fonts are named **OnAir CJK SC** in their name/CFF tables.

Source: https://github.com/notofonts/noto-cjk/tree/Sans2.004/Sans/OTF/SimplifiedChinese

Only Regular and Bold are shipped. The medium UI role shares Regular; semibold and
bold UI roles use Bold. The subsets retain all 7445 GB2312 characters, Chinese
interface characters available in the upstream fonts, ASCII, NBSP and the OpenType
layout glyph closure. Each face maps 7543 selected Unicode characters. The small
triangles U+25B4/U+25BE are absent upstream and continue to use the base/system fonts.
`gb2312.txt` is the sorted UTF-8 GB2312 repertoire used by the coverage regression test.

Subsetting removes unused glyphs/tables and renames the fonts; the retained glyph
outlines, horizontal metrics, units per em and line metrics are unchanged and verified
against the originals by the generation script. Chinese text in the retained repertoire
uses these fonts first. Other characters (including Hangul and Han characters outside
the subset) depend on the existing system-font fallbacks. No font downloads occur at runtime.

The CJK em is normalized to 90% of the base UI line height (the upstream fonts have
a 1000-unit em and a 1448-unit ascender/descender box), at every role and rendering density.
Latin text and numbers retain Inter / JetBrains Mono / the Classic system monospace font.

## Reproduce

Font subsetting is a maintainer step; ordinary builds only need Python's standard library.
Tested with fontTools 4.62.0 (`python3 -m pip install fonttools==4.62.0`). Download the two
original OTF files from the source above into a separate directory, then run:

```sh
python3 tools/subset_cjk_fonts.py /path/to/original-otfs
```

The script validates the original checksums below, derives GB2312 from Python's strict
`gb2312` codec, adds `app/i18n/zh.txt`, subsets with fontTools (the pyftsubset engine),
checks coverage and retained glyph outlines/metrics, and prints output sizes/checksums.
Re-run it when Chinese catalog characters change. `tools/embed_cjk_font.py` embeds the
resulting files as raw-DEFLATE resources during the build. There is a 10 MiB combined
OTF resource budget in the generator and typography test.

## SHA-256 of originals

- Regular: `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`
- Bold: `b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a`

## SHA-256 of shipped subsets

- `NotoSansCJKsc-Bold.otf`: `66c7d73c5d99aa5e29c14acfd2424613326e30c5d8f043dadee7fd5355ce2848`
- `NotoSansCJKsc-Regular.otf`: `4b43e89b29c0ef5c47ceb69491e2d35d2848b33f722a01a5b71114603f351104`
