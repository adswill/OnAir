# Fleet Broadcast (UHF SATCOM), 3.9

Mode 870 (engine standard 872, active standard 871, poll slot 870). Receive only. Own mode-list entry; the UHF SATCOM skimmer (family 552) only detects energy on 240 - 270 MHz and does not frame anything, so nothing was duplicated.

## What the public description says

Only one primary source exists, the r00t.cz page "Fleet Broadcast Decoder v0.5" (last modified 2009): Fleet Broadcast is a 16 channel 75 Bd multiplex from SATCOM satellites on 250.450 / 250.550 / 250.650 MHz. Modulation is DBPSK at 1200 Bd (receiver with a 2 kHz filter, SSB). Channel 0 is used for synchronisation; the other channels are usually filled with pseudo-random (encrypted) data, sometimes KG-84 messages, and "no plaintext was ever seen". 16 x 75 Bd = 1200 Bd, so a frame is 16 bits, one bit per channel. A US Navy FLTSATCOM shore-terminal description (electronicstechnician.tpub.com) agrees on 75 bit/s subchannels sent time-division multiplexed at 1200 bit/s (it counts 15 message subchannels, the r00t.cz page 16 with channel 0 for sync).

Licence: the r00t.cz page says "published for personal use only" and offers only a 41 kB binary (fbr.zip). No source, no licence that allows reuse. So nothing of it was used or ported (no entry in THIRD_PARTY.md or the credits); the page was read for the format description only. A 2026 blog post (i56578-swl) mentions Fleet Broadcast beacons but gives 2400 baud for a different signal and was not used.

## What is NOT public and so not claimed

- The sync pattern of channel 0 (and its length). The receiver does not check a sync word.
- The pulse shape, the bit polarity, any message framing inside a channel, which satellite carries which carrier, the KG-84 signature (not detected).
- Any text encoding: no plain text is known, so no Baudot or ASCII decoding is attempted.

## What the mode does

Full chain, simulated signals only: carrier acquisition (squared signal, +-4.5 kHz around the tuned frequency, about 0.6 Hz resolution, so satellite Doppler is covered), a carrier frequency loop (follows a drift of 20 Hz/s in the tests), 1200 Bd symbol clock (Oerder-Meyr line of the energy, first-order loop, clock error in ppm), differential detection (a phase change is bit 1; the "Invert bits" switch flips it), cutting the stream into the 16 columns.

Framing: every ~0.85 s the last 4096 bits are cut into 16 columns and each is classified: constant (idle, 90%), reversals 1010 (90% changes), repeating with a period of 3 to 64 frames at 80% agreement (sync pattern), or random-looking (encrypted-looking; not decoded). When exactly one column repeats it is taken for channel 0 and the other columns are numbered from it. That is an assumption, not a check against the real sync word: with a real signal whose sync channel does not repeat within 64 frames, the mode stays at "bits flowing" and shows the columns as they come; with several repeating columns it says the frame start cannot be told.

Parts: full decode of the modem and bit stream; framing by repetition (assumption); channel classification (detection only); message content: none (framing and fields only: the channels are shown as hex, 16-bit frame words, and a 16 x 24 bit matrix). Encrypted-looking channels are labelled so and left alone. Nothing is decrypted.

Panel (app/fbr): Channels (the 16 rows with class, change rate, share of ones, the last 64 bits in hex, and the bit matrix), Messages (event log and frame words), Link (carrier, clock, decision quality), Notes (sources and limits); carrier presets in the list and the tuning bar; the polarity switch. UI strings go through tr()/trw() with German entries (app/i18n/de.txt, listed in unreviewed/de.txt).

Radio: 250 ksps, 30 kHz tuning offset (keeps the DC spike off the carrier), minimum 38.4 ksps. Sample rates from 38.4 ksps up work (the integrate-and-dump factor is round(rate / 19.2 kHz)).

## Test signal and tests

core/src/fbr/fbr_gen.cpp: DBPSK at 1200 Bd with half-cosine edges, 16 channels of one bit per frame: channel 0 a 31-bit m-sequence of our own (x^5 + x^3 + 1, NOT the real pattern), channels 1 and 4 reversals, 2 idle ones, 3 idle zeros, the rest random. Options: SNR (symbol power over noise in 1.2 kHz), offset, clock ppm, slow fading, Doppler drift, polarity, five scenarios (mixed, sync + constant, noise only, plain carrier, random everywhere). Also the engine's test signal for standard 872.

tests/fbr/test_fbr_rx.cpp checks: the channel classes, sync repeat, hex of the idle channels, the frame words against the generator, offsets of -4000 .. +4000 Hz and with a -30 kHz radio offset, +-100 ppm clock error, 20 Hz/s Doppler drift, SNR 10 and 8 dB, 12 dB fading, a stream with constant channels, polarity, 48 ksps, the engine's test signal; and that noise alone, a plain carrier and random data on every channel never claim a frame. ONAIR_TEST_LONG=1 adds more offsets (to +-4400 Hz), +-250 ppm, SNR to 4 dB, 96 ksps and 1 Msps.

## Honest status

Everything above was validated on simulated signals built from the public description. No real recording was available, so nothing was compared with a real Fleet Broadcast signal: the sync pattern, the polarity and the pulse shape are unverified. The decoder is "framing and channel activity", not a message decoder.

CI: see the last commit body (quick.yml, test fbr_rx).
