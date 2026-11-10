# IoT and RC links in 3.9 (branch feat39-iotrc)

This work adds receive-only decoders. Every signal in the tests is simulated: no recording of any of these devices or links was available. No family numbers were needed, because each item extends an existing receiver. The 680-699 block is unused.

## Added

| Item | Where | Mode-list entry | Source |
|---|---|---|---|
| X10 RF, Insteon RF | ISM sensors (`core/src/ism/ism_dec_39.cpp`) | ISM sensors (auto-detected) | rtl_433 `x10_rf.c`, `insteon.c` |
| inFactory / nor-tec TH, Ambient Weather WH31E / WH31B, Nissan TPMS, GM aftermarket TPMS, Efergy Optical, ESIC EMT7110, Itron ERT SCM+ | same file | ISM sensors (auto-detected) | rtl_433 `infactory.c`, `ambientweather_wh31e.c`, `tpms_nissan.c`, `tpms_gm.c`, `efergy_optical.c`, `esic_emt7110.c`, `scmplus.c` |
| Wize (169 MHz) | Wireless M-Bus (`core/src/wmbus`) | Wireless M-Bus (Wize is EN 13757-4 mode N, recognised by CI 0x20) | Wize Alliance LAN Protocol Specification v1.5, Regional Parameters v1.2 |
| Futaba S-FHSS | RC links (`core/src/rclink/rc_proto.cpp`) | new entry "Futaba RC (S-FHSS)", sub-mode `sfhss` | DIY-Multiprotocol-TX-Module `Futaba_cc2500.ino` |
| Spektrum DSM2 / DSMX | RC links (`core/src/rclink/rc_dsm.cpp`) | new entry "Spektrum RC (DSM2 / DSMX)", sub-mode `spektrum` | DIY-Multiprotocol-TX-Module `DSM.ino`, `DSM_cyrf6936.ino`; gr-dsmx-rc; CYRF6936 datasheet |

The rtl_433 device sources are GPL-2.0-or-later, which is GPL-3 compatible. They are credited in THIRD_PARTY.md and in the in-app credits. Our code is our own; the frame formats and check algorithms come from those files. The ISM families were chosen by comparing rtl_433's device list with what the ISM mode already decoded.

### Wize
- PHY:
  - Mode N gains a 2.4 kbit/s variant (`Phy::N24`, Wize WM2400, GFSK +-2.4 kHz) next to N1 at 4.8 kbit/s, on all six 12.5 kHz channels from 169.40625 MHz.
  - The format B sync word F672 is accepted alongside F68D.
- Frame: a format B frame with CI 0x20 is parsed as a PRES-EXCHANGE L6 layer. The fields are L6Vers, L6KeySel, L6NetwId, L6Cpt, L6App, L7Ciph, L6HashKenc, L6TStamp and L6HashKmac.
- Encryption: the L7 data stays encrypted unless the key selector is 0. The Wize keys (Kenc, Kmac) are not used, and nothing is decrypted.
- Not done:
  - The 6.4 kbit/s 4GFSK rate (WM6400) and the LDPC long-range rates.
  - The L6 hashes are shown but not verified.

### Futaba S-FHSS
- PHY: 2-FSK at 128 kbit/s with 38 kHz deviation, sync D3 91, and a fixed 13-byte packet plus a CRC.
- Shown per packet: transmitter id, hop channel, hop code, packet number, and the four channel values (CH1-4 or CH5-8). Failsafe packets are flagged.
- The CRC is uncertain. Multiprotocol sets the CC2500's CC2400_EN bit, which selects the CC2400 CRC. The decoder accepts CRC-16 CCITT (init FFFF, which is what the generator writes) or the CC2500 CRC, and reports which one matched.
- A real capture must confirm which of the two CRCs is right.

### Spektrum DSM2 / DSMX
- PHY: CYRF6936 64-chip 8DR DSSS at 1 Mchip/s.
- Each burst is tried against all 40 SOP codes (5 pn rows x 8 SOP columns). The 19 symbols (length, 16 data bytes, CRC) are then despread, each to one byte:
  - bit 7 picks the data code
  - bit 6 picks the polarity
  - bits 0-5 give the cyclic shift
- Shown per packet: the two transmitter id bytes and the channel words, as 11-bit or 10-bit values with their channel numbers.
- DSMX is told from DSM2 by the pn row of the hop channel, which needs the radio's centre frequency. Without it the label is "DSM".
- The DSMX hop list is computed from a full transmitter id for the tests. The receiver only sees two id bytes, so it does not follow the hops.
- The CRC is not checked. The CYRF6936 CRC is 0x8005 with a seed, but how DSM applies it over the air is not published; gr-dsmx-rc did not match it either. A packet is accepted only when:
  - all 19 symbols despread within 14 chip errors
  - the length byte is 16
  - the channel words pass a strict format check (distinct channel indexes, FFFF for unused words, bit 15 only in the first word)
- In the test, 2000 random 16-byte packets were tried against the format check.
- Not done:
  - Bind packets (64-chip SDR without SOP). Their chip polarity is unverified.
  - DSMR and DSM2 SFC.
- Unverified assumptions:
  - The byte mapping comes from gr-dsmx-rc's decoder of real DX6i captures, not from the CYRF technical reference manual.
  - The GFSK polarity (1 = upper frequency) and the 250 kHz test deviation are assumptions.

## Skipped (no public description)
- **NB-Fi (WAVIoT):** the specification documents are for registered users only, and no open stack exists. Only the PHY rates are public.
- **Weightless-P:** the specification is for Weightless SIG members only.
- **Futaba FASST:** no packet-level reverse engineering is published. Multiprotocol does not implement it.
- **FlySky AFHDS 3:** only the module UART protocol is documented, not the air protocol. AFHDS and AFHDS 2A are already decoded.

## Tests
- **`tests/ism/test_ism_39.cpp`:**
  - Known answers: the WH31E CRC-8 and SUM-8, the SCM+ CRC 0x39BE and the GM sum, all from frames quoted in rtl_433's notes.
  - Every device's pulse train decodes with timing stretched by -8 % to +8 % and with 4 % edge jitter.
  - One moved edge gives no reading.
  - Over the air, scene 4 is run clean, with a +30 kHz tuning error, with a 200 ppm clock error, at 14 dB, and with noise alone. -30 kHz runs only with ONAIR_TEST_LONG=1.
- **`tests/wmbus/test_wmbus_wize.cpp`:**
  - The L6 fields round-trip, every 5th byte is flipped against the CRC, and a cut-off frame is refused.
  - Over the air on the 169 MHz plan, two Wize meters (2.4k encrypted, 4.8k in clear) and the N1 meter are run clean, at -1.5 kHz, at +1.2 kHz and at 20 dB. No frame may be wrong.
- **`tests/rclink/test_rclink_39.cpp`:**
  - S-FHSS: every hop channel and code round-trips, the hop sequence is checked, and every flipped bit is rejected. Over the air it runs at +-300 kHz with noise.
  - DSM: all 256 bytes despread for all 40 code sets, also with 8 chip errors. The DSMX hop list rules hold, 10-bit and 11-bit packets round-trip, and the format check is tried against random packets.
  - DSM over the air: offsets of -2 and +1.5 MHz, 100 ppm clock error and noise, with DSMX and DSM2 told apart.
  - Noise alone gives no frame. Longer sweeps run with ONAIR_TEST_LONG=1.

Simulated only: none of these decoders has been checked against a real signal.
