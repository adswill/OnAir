# Issues 2026-10-24: four small features for 3.1 (branch issue-feat-031, from v3.0.0)

| # | title | request in one line | status | commit / branch | not verified | reply draft |
|---|---|---|---|---|---|---|
| 165 | HD Radio decoding | long station names and logos shown | done before (3.0: long name first, per-service logos, LOT assembly; 79ad4ff37). No new code | 79ad4ff37 | real HD Radio signal | R/165-hd-radio-names-logos.md |
| 225 | Hellschreiber Unicode | Chinese, Japanese, emoji in the modulator | added now: optional user font file (.ttf/.otf, Advanced), drawn 10 px high; nothing bundled, so no licence notice needed | issue-feat-031 | on-air; the decoder round trip of a CJK glyph (glyphs are checked, not the audio) | R/225-hellschreiber-unicode.md |
| 233 | SDR gain stages | every advertised gain stage as a control | added now: Airspy lna/mixer/vga, SDRplay LNA state + IF reduction (Soapy elements, HackRF, Fobos LNA, RTL AGC were there); -1 follows the slider | issue-feat-031 | real Airspy / RSP (fake driver libraries only); RTL has no per-stage call | R/233-gain-stages.md |
| 236 | Whole span for VDL2 / HFDL | automatic full-capture switch | added now: Whole span button for both; VDL2 worldwide channel set + list, HFDL table + list | issue-feat-031 | real wideband capture | R/236-whole-span-vdl2-hfdl.md |

R = /private/tmp/claude-501/-Users-kristijonasadomaitis-dvb-t2-decoder/6c46c5ab-dc6f-4964-bf96-43f9e5f83544/scratchpad/replies-3.0
Tests: test_vdl2_rx (whole span channel sets), test_hfdig_kbd (user font glyphs), test_native (Airspy / RSP stage settings).
Limits: the user font is drawn at 10 pixels height (the receiver syncs on the blank rows), so complex CJK glyphs are coarse; colour emoji fonts do not work.
