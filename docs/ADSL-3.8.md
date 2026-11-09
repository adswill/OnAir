# ADS-L in 3.8

ADS-L is the EASA / SESAR iConspicuity link for light aircraft, gliders, paragliders and drones. Written from the EASA specification (ADS-L 4 SRD-860, Issue 2, ED Decision 2022/024/R); OGN `ads-l.h` as carried in SoftRF (GPL-3) was read for what the document leaves open, no code copied.

**Nothing here has been validated on real signals.** No public IQ recording of ADS-L was found (two searches, the listed implementations, the OGN and stratux trees). Every test is loopback against our own generator or the specification's worked examples. A user test on a real ADS-L tracker is still needed.

## On the air

| channel | frequency | modulation | rate | preamble | sync |
|---|---|---|---|---|---|
| M-band | 868.2 / 868.4 MHz | GFSK BT 0.5, +-50 kHz | 100 kchip/s, Manchester (data 50 kbit/s; 1 = chips 01) | (01) x 5..12, 1001 1001, not Manchester | 0x72 0x4B (Manchester) |
| O-band LDR | 869.525 MHz | GFSK BT 1.0, +-12.5 kHz | 38.4 kchip/s | (10) x 40 | 0xB4 0x2B, then the fixed header 00 00 00 00 18 71 |
| O-band HDR | 869.525 MHz | GMSK BT 0.5 | 200 kchip/s | (10) x 12 | 0x2D 0xD4 |

Packet: length byte, network header (version, signature flag, key index, parity type), ADS-L data (type, 30 bit address with its table, relay flag, payload), parity. A CRC-24 (the Mode S polynomial, most significant byte first) or Reed-Solomon (255,223; field 0x11D, primitive element 11, first root 121; 32 parity bytes, short blocks padded in front). O-LDR has a fixed length and a CRC-8 besides. Multi-byte fields are little endian; bit offsets count from the least significant bit.

Data is scrambled with XXTEA, 6 rounds, key 0 (all zeros, published), or not at all with key index 3. Key indices 1 and 2 are reserved and no such keys exist yet, so **no key is needed and none is imported**; such packets are counted. An optional Ed25519 signature is recognised and ignored: no public key infrastructure exists.

Payloads: Traffic (position, altitude above WGS-84, ground speed, climb, track, category, emergency, quality), Status (capabilities), Traffic Uplink (up to 10 relayed targets, usually FEC on HDR), FIS-B and Remote ID (counted only).

Where the document and practice differ: the specification lists `num_data_words 6`, but a Traffic data block is 5 words (40 + 120 bits); the OGN code and our receiver scramble 5 words, computed from the length. Issue 1 used a different LDR modulation and is not followed. OGN and SoftRF trackers send climb 0x100 ("minus zero") for no climb rate; it is taken as invalid beside the 0x1FF of Issue 2. The CRC-8 of O-LDR is not checked on receive: which bytes it covers is not settled, the CRC-24 decides.

## Receiver

`core/src/adsl`: per channel mix, low-pass, resample to 8 (HDR 4) samples per chip, FM discriminator, integrate-and-dump at every phase, sync hunted at every phase in both polarities, the same chain as the FLARM receiver. Default tune 868.875 MHz at 2 Msps holds every channel; 1 Msps at 868.3 MHz holds the M-band only, and channels outside the capture switch off by themselves. The FLARM receiver is fed from the same capture in this mode (`flarmOffsetHz`), so one mode shows both on one map and table.

Repair: a single wrong Traffic packet is fixed from the CRC syndrome, up to four weakest bits plus one more. A repaired packet only updates an aircraft already heard cleanly, never creates one.

Sharing: aircraft go to the shared map like FLARM. Aircraft on a random privacy address (table 0) are shown locally and not shared.

Mode numbers: family 300, engine standard 302, active standard 301, poll slot 260.

## Tested (loopback only)

`tests/adsl/test_adsl_proto.cpp`: spec examples for altitude, speed and climb coding, CRC-24 on a known Mode S frame, XXTEA against the specification's routine, Reed-Solomon to 16 errors, 113 664 field and network parameter combinations.

`tests/adsl/test_adsl_rx.cpp`, each of the four channels: every key index, parity and version, shortest and default preamble; carrier offset to +-20 kHz (LDR +-10 kHz) with clock +-40 ppm; inverted spectrum; noise sweep 25 to 8 dB (M-band full to 12 dB, HDR full to 14 dB in a 200 kHz reference); colliding packets, simultaneous channels, a 20 dB stronger adjacent channel, back-to-back packets; garbled length byte; 1 to 3.2 Msps and different centres; noise alone gives no packet; FLARM in the same capture decodes.

`test_mode_numbers`, `test_gui_rules`, `test_credits` pass. The `onair_adsb` crate tests pass, with a regression test for the TIS-B fix below.

**Not run**: the app itself and the GUI (no full build was allowed on this machine). Reed-Solomon and uplink formats are checked only for self-consistency.

## Related fix

DF18 with control field 3 (coarse TIS-B) was being read with the ADS-B layout, giving false positions. `rust/onair_adsb/src/msg.rs` now skips it, as readsb does (`mode_s.c`, case 3 returns before the ME switch). Fine TIS-B (CF 2) with IMF 1 still reaches the tracker with its Mode A / track-file address treated as ICAO; unchanged here.
