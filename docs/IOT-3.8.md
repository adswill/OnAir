# IoT protocols, 3.8 (branch feat38-iot)

Receive only. Nothing here decrypts anything, follows a connection or transmits. Key fobs and vehicle access are not decoded (the ISM list keeps them out on purpose).

## What was added

| Protocol | Where | What it decodes | Validated on |
|---|---|---|---|
| KNX RF 1.1 | ISM sensors (`core/src/ism/ism_dec_iot.cpp`, `decKnxRf`), 868.3 MHz, S-mode PHY, 32.768 kchip/s Manchester | serial number, source address, destination or group address, hop count, length, TPCI / APCI, data bytes, CRC of every block | 198 generated frames (every length of one to three data blocks, both address kinds, jitter, rate error, every single-bit error rejected); **10 of the 11 real recordings of KNX-RF in merbanan/rtl_433_tests (868.32 MHz, 1.024 Msps) decode through the ISM receiver**, the 11th has a very noisy burst |
| HomeMatic BidCoS | ISM sensors, `decHomeMatic`, 868.3 MHz, 2-FSK 10 kbit/s | sync 0xE9CA, length, CC1101 CRC-16, byte de-obfuscation, counter, flags, message type, source and destination address, weather / remote / sensor / ack-status / switch events | simulated only: 832 generated frames, both polarities, spikes, every single-bit error rejected, 6000 noise trains |
| EnOcean ERP1 | ISM sensors, `decEnOcean`, 868.3 MHz ASK 125 kbit/s | RORG, sender ID, status (repeater count), CRC-8; RPS rocker and energy bow, 1BS contact, 4BS teach-in with EEP and manufacturer, other data as bytes | simulated only: 240 generated telegrams, every chip error rejected |
| DASH7 | ISM sensors, `decDash7`, two entries: 55.555 kbit/s and 9.6 kbit/s GFSK | PN9 whitening, CRC-16, background frames (subnet, tag, ETA), foreground frames (subnet, target ID, EIRP, origin class and ID, payload bytes; AES-secured frames show the method only) | simulated only: 288 generated frames, every single-bit error rejected. The FEC channel (convolutional code, sync words 0xF498 / 0x192F) and the 166.667 kbit/s class are not decoded |
| Bluetooth Classic discovery | new mode, family 380 (`core/src/btc`, `app/btc`) | access codes of every packet (LAP = piconet master or paged device), inquiry IDs (GIAC / LIAC), inquiry responses (FHS: full address, class of device, clock) | simulated only, see below |

EnOcean needed a front-end change: the ISM receiver cut ASK with a 20 us average (nothing shorter than about 100 us), so 8 us chips never decoded. When the narrow envelope gives a train that no protocol reads, the receiver now slices the channel envelope itself and keeps that train only if a protocol reads it. The old ISM receiver test (`ism_rx`, 14 + 5 + 3 devices, offsets, clock errors, noise, collisions, cut-off packets) gives the same results.

### Already in the Sensors mode (rtl_433), not added again

Sensors mode (family 241) vendors the whole rtl_433 device list. It already has **Insteon** (159), **EnOcean ERP1** (198, off by default, needs at least 1 Msps) and **KNX RF** (inside `m_bus.c`). What this branch adds to the ISM sensors mode is the same or more: for KNX RF decoded addresses and fields, for EnOcean the RORG-level fields instead of a hex string. `core/src/ism/ism_dec_energy.cpp:145`, which discarded KNX RF frames, is now a comment pointing to `decKnxRf`: the M-Bus T / C decoder still skips them because KNX RF has its own physical layer.

## Bluetooth Classic discovery (mode 380)

- A wide capture (default 20 Msps at 2441 MHz, minimum 4 Msps) is watched in FFT frames of 6 to 8 us for each 1 MHz channel. A channel that rises 10 dB over its floor starts a burst; a run of five or more adjacent channels at similar levels is Wi-Fi and is skipped; the neighbours of one strong narrow signal are its leakage and are skipped.
- Each burst is cut out, mixed to 0 Hz, filtered (600 kHz) and resampled to 4 Msps, FM-demodulated, sliced at four sample phases, and searched for a 72-bit access code: the 7 Barker and LAP-top bits, then the sync word of the LAP found in it with at most two bit errors, then the alternating preamble. The 64-bit sync word carries the LAP (24 bits), so any packet names its piconet master or the device being paged; no key, clock or UAP is needed.
- An access code with the GIAC and a trailer is an inquiry response: its FHS packet (FEC 1/3 header, HEC with UAP 0, FEC 2/3 payload, CRC-16) is decoded for all 64 whitening seeds, giving the full address (NAP:UAP:LAP), class of device (major and minor class, service classes) and clock. Names are not in these packets.
- The radio hops 1600 times a second over 79 channels, so a 20 MHz capture hears about a quarter of the packets of a device; discovery fills in over a minute or two.
- Not done: packet payloads, connection following, UAP recovery by HEC brute force from ordinary packets, Enhanced Data Rate payloads (the access code of those packets is found like any other), Bluetooth LE (its own mode), inquiry by OnAir (never transmits).

Numbers: family 380, engine standard 382, active standard 381, poll slot 380. `kNumFamilies` is 381 and `kPollSeqSlots` 381 in this branch; the integrator takes the larger value of every branch.

### Validation (all on simulated signals)

- Sync word: the GIAC sync word equals the value of the Bluetooth specification (0x475c58cc73345e72); 2000 random LAPs gave the same sync word as libbtbb's generator.
- FHS packets built by the generator (all 64 whitening seeds, random fields) were decoded by libbtbb (Ubertooth, GPL-2.0-or-later; compiled for the check, not shipped): 300 of 300 with identical LAP, UAP, NAP and clock. Our own decoder: 512 of 512; 4000 damaged packets (1 to 12 bit errors): 1967 decode, none with wrong values; 20000 random bit strings: no FHS. 10 million random bits: no false access code.
- Over the air (`test_btc_rx`, synthetic GFSK 1 Msym/s, BT 0.5, index 0.32, white noise): 20 / 10 / 8 / 4 Msps, centre 2441 and 2438 MHz, radio offsets of +-50 and 100 kHz on top of the device offsets of up to 55 kHz, 22 / 16 dB C/N in 1 MHz (everything heard, nothing wrong), 10 dB (some access codes, nothing wrong), a 20 MHz Wi-Fi-like burst every second (skipped), cut-off packets (nothing wrong), a device 30 dB louder on a neighbouring channel (heard, nothing else disturbed), noise and zeros (no hits).
- No real recording of Bluetooth Classic, no live signal, and no HackRF test. The access code detector and the FHS decoder follow the specification as libbtbb implements it; the radio front end and the burst detector were only ever tried on the generator's signal. Expect to tune thresholds against a real 2.4 GHz band (Wi-Fi and BLE traffic).

## Skipped, and why

| Item | Reason |
|---|---|
| Lutron ClearConnect (Type A, 433 MHz; Type X, 2.4 GHz) | No public protocol. The only description is one person's reverse engineering (CRC polynomial and packet layout from a single blog post, not confirmed elsewhere); remotes are also paired and not meant to be read by third parties. |
| Wireless DMX (CRMX by LumenRadio, W-DMX by Wireless Solutions) | Both are proprietary radios without a published air interface. |
| mioty (TS-UNB, ETSI TS 103 357) | The physical layer is public, but a telegram is 24 sub-packets of about 2.5 kHz spread over 200 kHz by a hopping pattern, and the payload is AES-encrypted; it needs a coherent multi-sub-packet receiver with its own test signal. Not attempted in this branch. |
| DECT NR+ (ETSI TS 103 636) | The specification is public, but decoding needs the OFDM numerology, the polar code and the HARQ / MAC layer; no recording or transmitter to test against. Not attempted. |

## Numbers for the record

- `tests/ism/test_ism_iot.cpp` (train level), `tests/ism/test_ism_iot_rx.cpp` (over the air, scene 3 of `ismTestScene`: KNX RF, HomeMatic, EnOcean, DASH7 at 868.3 MHz; clean, +-50 kHz, 20 dB and 14 dB C/N, cut-off packets, a collision: all heard clean, nothing wrong anywhere; at 14 dB EnOcean is marginal), `tests/btc/test_btc_proto.cpp`, `tests/btc/test_btc_rx.cpp` (slow tier).
- Nothing was built with CMake and no full test run was made locally; the tests were compiled and run standalone against the prebuilt libraries. The engine and app files were checked with `clang++ -fsyntax-only` only.
