# Bundled fonts

The Inter and JetBrains Mono families are licensed under the SIL Open Font License 1.1 (full text: `LICENSES/OFL-1.1.txt`). Neither declares a Reserved Font Name.

| Font | Copyright | Version and source | Files here |
|---|---|---|---|
| Inter | Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter) | 4.1, https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip (static `extras/ttf`) | `Inter-Regular.ttf`, `Inter-Medium.ttf`, `Inter-SemiBold.ttf` |
| JetBrains Mono | Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono) | 2.304, https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip | `JetBrainsMono-Regular.ttf`, `JetBrainsMono-Bold.ttf` |

The files are subsets (Latin, Latin Extended, Greek, Cyrillic, punctuation, arrows, maths, box and block characters; hinting removed) made with fontTools `pyftsubset`, to keep the program small. A modified copy is allowed by the OFL.
`tools/gen_typo_fonts.py` turns them into `app/ui/typo_fonts.inc` (deflate-compressed arrays, inflated at start-up).

Noto Sans CJK SC supplies Chinese text on every desktop platform; the unmodified full fonts, provenance, checksums and OFL license are in `noto-cjk/`. They are embedded at build time (Python 3 required); generated C++ stays in the build folder.
