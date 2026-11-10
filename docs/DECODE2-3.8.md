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
| Meshtastic | Waypoint, air quality and power telemetry, range test, alert, detection sensor, reply, paxcounter, store and forward, map report, position speed/track/HDOP | meshtastic/protobufs | rust/onair_mesh tests |
| EBICab | SUBSET-026 packets 5, 21, 27, 41, 65, 68, 72, 79 after the header; other packets are skipped by their length | openETCS XML of chapter 7 (Baseline 3) | tests/ebicab/test_ebicab_packets.cpp |
| NDB census | One decoder per carrier across the capture, table in the NDB tab | Existing NDB decoder | tests/ndb/test_ndb_census.cpp: 3 beacons, 22 and 35 dB |
| TACAN | Radial from main and auxiliary bursts and 15/135 Hz amplitude modulation | MIL-STD-291C bursts; the east-referenced convention from secondary descriptions | tests/dme/test_dme_tacan.cpp: within 0.2 degrees in simulation; direction of rotation not confirmed on air |
| User keys | Zigbee NWK, Wi-SUN (key is the GAK), Z-Wave S0, wM-Bus mode 5, LoRaWAN MIC and FRMPayload. Keys come only from the key store | Wireshark, Grizzly and arXiv papers, wmbusmeters, LoRaWAN spec | tests/voicekeys/test_net_crypto.cpp, tests/lorawan/test_lorawan_crypto.cpp: RFC 4493, RFC 3610 and the lora-packet example frame pass; wrong keys rejected |
| LoRaWAN Class B | EU868 beacons (869.525 MHz SF9 implicit, 17 bytes) | LoRaWAN 1.0.2 chapter 15 | The spec's example frame, and through the LoRa bank at 5 and 25 dB |
| fldigi RSID | Mode and audio frequency, starts the matching keyboard decoder | fldigi rsid.cxx | tests/hfdig/test_hfdig_rsid.cpp |
| FX.25 and IL2P | Receive on the 1200 baud demodulators, frames join the normal packet list | Dire Wolf, IL2P draft 0.6 | tests/packet/test_packet_fec.cpp at bit level; the receiver wiring has only a syntax check |
| WSPR balloon | Altitude, locator letters, temperature, battery, speed from telemetry spots | QRP Labs S4 page | tests/hfdig/test_hfdig_balloon.cpp. The page's worked example prints 36 C and 3.8 V; the same formulas give 38 C and 3.85 V, so one of the two is wrong |
| FIS-B TWGO | Text and overlay records (products 8 to 16), overlay points listed as text | fisb-decode | tests/uat/test_uat_twgo.cpp |
| GOES DCS relay | Band selector for 1679.85 MHz with the same decoder | NOAA frequency plan | Assumes a bent-pipe relay. One wiki and an old plan say 8PSK on the downlink, so unconfirmed |
| Galileo OSNMA | NMA header only: service status, chain id, chain and key status, in the log | OSNMA SIS ICD 1.1 | No authentication is done |

Limits worth knowing: iMS-100 temperature and humidity need the calibration table and are not shown. MRZ and iMet-54 do not report all sensor values. SIB6 to SIB8 are still not parsed, so a SIB10 or SIB11 placed after one of them in the same SI message is not read; SIB12 and SIB16 are unaffected.

## Not done

- Galileo OSNMA authentication (DSM-KROOT signature, TESLA chain, MACs). Only the header is read. Needs the MAC look-up table, ADKD message assembly and ECDSA, and there is no signal to check it against.
- 5G SIB1: needs PDCCH blind decoding, DCI polar decoding, PDSCH and an LDPC decoder with the 38.212 base graphs. Not started.
- SARSAT second generation rotating fields: C/S T.018 could not be read. A mirror and search results gave only the table of contents.
- Extra ADS-B Comm-B registers: ICAO Doc 9871 is sold, and the open decoders only cover the registers already implemented.
- Z-Wave S2 (needs the session key exchange), wM-Bus mode 7, FX.25/IL2P at 9600 baud, FLMSG, TWGO on the map and segmented APDUs.
- LoRaWAN joins: join-accept decryption is not implemented; session keys must be supplied.

To use LoRaWAN keys, add two entries in the Keys window with protocol LoRaWAN and Unit = DevAddr (decimal): talkgroup 1 for NwkSKey, talkgroup 2 for AppSKey.
