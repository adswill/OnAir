# Smart voice package 3.7 (hamvoice proposals 6 - 11)

Branch `feat37-sm-voice`. Every number below is measured by the test named next to it.

| # | Feature | State | Measured |
|---|---------|-------|----------|
| 6 | Soft-decision AMBE frame FEC with concealment (`core/src/voice/vocoder_fec.cpp`, wired into DMR, D-STAR, NXDN) | done, on by default | AWGN sweep, `test_voice_fec`: at sigma 0.75 garbage frames reaching the codec fall from 17.2 % to 1.9 % (sigma 0.65: 7.3 % to 0.4 %, sigma 0.85: 28.8 % to 3.6 %); the frames still unsure are concealed (repeat, fading gain, mute) instead |
| 7 | Symbol equaliser for the 4-level links (`core/src/voice/sym_eq.cpp`, DMR link) | done, **off by default** | Symbol level, `test_voice_symeq`: echo 0.5 symbol 39 % to 0.9 % errors, simulcast-like 43 % to 0.5 %, a clean channel left alone. Through the real FM front end (`test_dmr_equalizer`, 32 echo scenarios) no net gain (-0.5 %), so it stays off until the front end gives a linear symbol stream |
| 8 | Heard-ID repair (`voice/heard_ids.h`, D-STAR header, DMR voice LC) | done | `test_voice_heard`: D-STAR 1 or 2 damaged characters 0 % to 100 % of headers back, 3 characters 95 %; DMR LC with three flipped ID bits 949 of 1000 back (51 fixed by Reed-Solomon alone); 0 wrong repairs in every case, because a candidate counts only when CRC / RS then passes |
| 9 | Trunk call dedup across sites (`PoolConfig::dedupSites`) | done, on by default | `test_trunkpool_dedup`: 40 pairs of simultaneous calls each granted by 2 sites, 3 radios: 160 call rows become 80; a clearly better-heard copy in the first 2.5 s takes the call over, no flip-flop. P25 grants carry the site from the controller |
| 10 | Control channel health + auto parking (`trunkpool/cc_health.cpp`, `trunk::Controller`) | done, on by default | `test_trunkpool_health`: a channel degrading to 60 % bad is left after 90 s; two equal noisy channels over an hour give 0 moves. Site book (adjacent sites learned) with CSV export |
| 11 | TETRA SDS / LIP interpreter (`core/src/tetra/tetra_sds.cpp`) | **parser only** | `test_tetra_sds`: LIP position round trip within the 1.1e-5 degree step over 2000 random positions, text, truncated and random bits never give a position. The hook from the MAC-RESOURCE walker (Rust crate) and the map/call-log are not done: layouts are from memory of EN 300 392-2 / TS 100 392-18-1 and need a real capture |

Not done: dPMR and P25 phase 1 / 2 do not use the soft frame path yet (the API `NxdnAmbe::decodeFrSoft` is there); IMBE (P25) needs Golay + Hamming; Settings > Advanced switches (`voicefec::setEnabled`, `symeq::setEnabled`, `PoolConfig::dedupSites`, `Controller::autoPark`) are not in the UI.
