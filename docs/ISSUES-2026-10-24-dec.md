# Decoder issues round, base v3.0.0 (branch issue-dec-031)

All 14 checked against v3.0.0 (git merge-base --is-ancestor on each fix commit). No code change was needed; nothing was posted or closed.

| # | title | request | status | commit/branch | not verified | reply draft |
|---|-------|---------|--------|---------------|--------------|-------------|
| 277 | analog TV slow | TX generator 3-4 s per second of content | partly (done before: test card skipped when live frame; resampler multi-core; bench 2-5x real time on Apple silicon) | ea9dfcc0a | reporter's CPU/rate/settings unknown; needs owner (screenshot of parameters) | 277-atv-tx-slow.md |
| 275 | CDR decode failed | 98.2 MHz IQ does not decode / lock | not fixed; reproduced: file is 768 ksps (7708672 samples = 10 s + 28672), cdrtool never locks (no SDC); spectrum = FM + two ~30 kHz digital blocks at 162-192 kHz (likely spectrum mode 9/10 hybrid); sync search only returns false inner-band fits, outer-band modes 9/10 reach metric 0.66 max. Tried choosing the mode by pilot coherence instead of sync energy: no change, reverted | none | cause not found; needs deeper work or a second recording / station parameters | 275-cdr-decode.md |
| 273 | TETRAPOL polarity | automatic INV/NOR | done before | 966a7231a | real network | 273-tetrapol-polarity.md |
| 267 | TETRA echo | slot / carrier / 25 kHz choice, no echo | done before | a528ef196 (+#259) | real recording | 267-tetra-echo.md |
| 262 | DAB tuning fault | click/drag in wide capture | done before | 48ccfb1f4 | real radio | 262-dab-tuning.md |
| 249 | DAB transmitter list | list + map after scan | partly (list/ids/levels, import, export, full-height map done; TII decode tested incl. every-frame mode; no German site data, licence) | bd3e45bf4, 153a088dd | real German signals; needs owner/crowd site list (OnAir-channels dab-tii has FR only) | 249-dab-tii.md |
| 254 | DTMB choppy | stutter on Airspy R2 | done before (faster LDPC, GPU option) | a2c563273, a158ae625 | reporter CPU | 254-dtmb-choppy.md |
| 261 | pager decoding | FLEX / POCSAG no decode | done before (FLEX_NEXT; reporter djs007 confirms 3.0) | flexnext-030 merge, c74b2ab7a | original US poster not confirmed | 261-pager-decoding.md |
| 257 | KiwiSDR stopped sending samples | connect fails | done before | 3fd0c8578 | real Kiwi | 257-kiwi.md |
| 247 | DRM xHE-AAC | cannot decode | done before (user-installed libfdk-aac; state line, Look again) | 988531d30 | Windows, real CNR-1 | 266-247-xhe-aac.md |
| 266 | activate xHE-AAC | dll in codecs folder ignored | done before (codecs folder searched) | 2dfbe2342, 988531d30 | Windows | 266-247-xhe-aac.md |
| 252 | DMR audio | enable on Linux | done before (help showed codec2 state, fixed; docs/CODECS.md; voice in Calls tab) | 5a302eac1 | Linux box | 252-dmr-audio.md |
| 210 | DAB+ crash RTL-SDR v3 | crash | done before (buffer read before start, regression test) | ed4c053e8 | reporter retest on 0.2.9+ | 210-dab-rtl-crash.md |
| 223 | Cospas wider bandwidth | 100 kHz, fewer false decodes | done before | ec1bf9292, 94e47accf | more recordings | 223-cospas.md |

Counts: done before 11, partly 2 (#277, #249), open 1 (#275, reproduced, not fixed), fixed now 0, added now 0, not possible 0.
