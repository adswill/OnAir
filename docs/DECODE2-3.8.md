# Decoder additions, part 2 (3.8)

Everything marked "done" decodes its generator's output in a unit test. Nothing here was checked against a real off-air recording, because no recording of these message types was available: all results are simulated only, except the Ruuvi format, which matches the manufacturer's published test vector.

## Done

| Item | What changed | Layout source | Test |
|---|---|---|---|
| Cell public warnings | SIB10, SIB11, SIB12 (ETWS, CMAS, EU-Alert) reassembled per cell, text decoded (GSM 7-bit, UCS2, 8-bit), new Warnings view. SIB9 home eNB name, SIB16 network time. SIBs after the extension marker are now walked instead of ending the parse | TS 36.331 ASN.1, TS 23.041, TS 23.038 | tests/cell/test_cell_pws.cpp: UPER round trip, truncation at every length, segments in every order, through the LTE receiver at 25, 12 and 6 dB SNR with 0, +7.3 and -12.1 kHz offset |
| Radiosondes | iMet-1/4 (AFSK tones added to the symbol chain), iMet-54, MRZ-N1 (ECEF and lat/lon frames), Meisei iMS-100 (BCH(46,34)) | zilog80's RS decoders, GPL-3.0, credited | tests/sonde/test_sonde_extra.cpp and _rf.cpp: both polarities, bit errors, +-20 kHz offset through the full receiver. iMet-1/4 and iMS-100 decoded every frame at 14 dB in 10 kHz; iMet-54 is reliable from 22 dB and partial at 18 dB; MRZ from 18 dB |
| UAT NEXRAD | Products 63 and 64 parsed into blocks and shown as a Radar tab | dump978 extract_nexrad.c, GPL-2+ | tests/uat/test_uat_nexrad.cpp |
| ADS-B | TC 31 capability and operational mode, TC 19 NACv, surface length code, TC 28 subtype 2 RA broadcast joined with the Comm-B RA | readsb, DO-260B field layout via readsb | rust/onair_adsb tests |
| Galileo SAR RLM | Short and long return link messages from the I/NAV SAR field, reported in the log | Galileo OS SIS ICD issue 2.1 | tests/gnss/test_gnss_galrlm.cpp |
| BLE sensors | BTHome v2, pvvx ATC custom format, Ruuvi RAWv2 | bthome.io, pvvx README, docs.ruuvi.com | tests/ble/test_ble_sensors.cpp |
| Wi-Fi clients | Probe requests, data-frame association, deauth flood alarm, EAPOL-Key frames counted only. Addresses shown as salted per-run hashes | IEEE 802.11 | tests/wifi/test_wifi_clients.cpp |
| APRS | PHG, RNG, !DAO! precision | APRS101, WB4APR datum note | rust/onair_ax25 tests |
| Yaesu Fusion | GPS position and radio model from the slow data | YSFGateway, GPL-2+ | tests/fusion/test_fusion_gps.cpp |
| Meshtastic | Waypoint (port 8), air quality and power telemetry variants | meshtastic/protobufs | rust/onair_mesh tests |

Limits worth knowing: iMS-100 temperature and humidity need the calibration table and are not shown. MRZ and iMet-54 do not report all sensor values. SIB6 to SIB8 are still not parsed, so a SIB10 or SIB11 placed after one of them in the same SI message is not read; SIB12 and SIB16 are unaffected.

## Not done

Skipped because the spec or layout could not be read:

- SARSAT second generation rotating fields: T.018 could not be fetched from any public source I could reach.
- UAT FIS-B graphics other than NEXRAD (TWGO overlays): DO-358B is not public and dump978 does not decode them.
- ADS-B Comm-B extra registers: ICAO Doc 9871 was not available to check against.
- WSPR balloon telemetry: the U4B page describes fields but not the numeric mapping; the S4 page was not read.

Not started, for lack of time (no technical blocker found, unless stated):

- 5G SIB1: needs PDCCH blind decoding, polar DCI, PDSCH and an LDPC decoder with the 38.212 base graphs. Moved to the end because of its size.
- Galileo OSNMA: needs TESLA chain, HMAC and ECDSA with the imported key. The ICD is available.
- GOES DCS relay at 1679.7 MHz, EBICab SUBSET-026 packets, NDB census, TACAN bearing, Zigbee/Z-Wave/Wi-SUN/wM-Bus decryption with user keys, Meshtastic other ports, LoRaWAN Class B and user keys, fldigi RSID, FX.25 and IL2P.
