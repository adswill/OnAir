# Modes in detail

## Families, in more words

| Family | Modes | What you get |
|---|:---:|---|
| **TV** | 17 | DVB-T/T2, DVB-S/S2/S2X, DVB-SH, DVB-C2, ISDB-TSB, ISDB-S, TV sat beacon decoder, DVB-C, ATSC 1.0 and 3.0, ISDB-T, DTMB, analogue TV and MAC, analog FPV video (5.8 / 1.2 GHz), with a live player (picture, sound, subtitles, teletext, programme guide), recording, pause and live buffer, and 2-3 adjacent channels from one wide capture |
| **Broadcast radio** | 9 | DAB+ (with a programme guide), DRM (with its data services), HD Radio, CDR, AM broadcast (medium wave and shortwave), AM stereo (C-QUAM), and FM with RDS, a Mono switch for weak stations and a saved-station library |
| **Aviation** | 30 | ADS-B with a live map, ASTERIX radar data from UDP or a file, Mode A/C squawks and altitudes, UAT, ACARS, VDL2 and VDL4, HFDL, airband, VOR / ILS / NDB, FLARM, ADS-L, drones |
| **Maritime** | 8 | AIS (also long-range and satellite AIS), VDES, NAVTEX, DSC, weather fax, Marine VHF voice |
| **Satellite and navigation** | 57 | Weather pictures from Meteor, NOAA (APT, DSB), GOES, Elektro-L and Arktika-M (RDAS), FengYun-4 (LRIT, HRIT), FengYun-2 (S-VISSR) and MetOp, Iridium, Inmarsat (including BGAN), Orbcomm, QO-100, CubeSat telemetry, GNSS, SBAS corrections, COSPAS-SARSAT 406 MHz distress beacons, GOES / Meteosat DCS platform reports, Argos platforms and downlink |
| **Two-way radio** | 22 | DMR, P25 (phase 1 and 2, with a trunking follower), NXDN, dPMR, D-STAR, M17, TETRA, TETRAPOL, tone decoders (CTCSS, DCS, DTMF, selcall) |
| **Cellular and rail** | 12 | LTE, 5G NR and NB-IoT cells, GSM-R, LBJ, KLW, TDCS |
| **Pagers** | 4 | POCSAG, FLEX, Multitone |
| **IoT and sensors** | 43 | ISM sensors, LoRaWAN, Meshtastic, MeshCore, Bluetooth LE, BLE tracker detector, Zigbee, Thread, Sub-GHz sensors, Wi-Fi, Wi-SUN, Wireless M-Bus, Z-Wave, DECT, ANT+, Sigfox, NFC / RFID, Flipper Zero Sub-GHz chat |
| **Weather and science** | 56 | Radiosondes, Horus balloons, NOAA HRPT and GAC, Meteor HRPT, AWS, FY-3 AHRPT and MPT, SAME alerts, time signals, e-Czas, ionospheric sounders, radio astronomy, meteor scatter, Sferics (lightning) |
| **Amateur** | 52 | FT8, FT4, WSPR, JS8Call, RTTY, PSK, MT63, FSQ, IFKP, ARDOP, SSTV, FreeDV, APRS, LoRa APRS, MeshCom and more |
| **HF utility** | 41 | ALE, beacons, Datawell buoys, CODAR, STANAG 4539, 3G-ALE, Link 11 CLEW, STANAG 4481, CIS data modes |
| **Tools** | 29 | General receiver, spectrum analyser, spectrum explorer, signal workbench, measurement meters (MER, SINAD, noise figure, phase noise, occupied bandwidth, intermodulation and more); also a gain optimiser, a direction finder, a pass planner and a remote-control API |
| **KA family (own modes)** | 6 | KAAL, KAPP, KAFT, KAVB, KAIT, KAWS: digital modes designed for OnAir, in development (see below) |

The compact family table is in the [README](../README.md#modes); the full numbered list is [MODES.md](../MODES.md). The KA family has its own page: [KA-FAMILY.md](KA-FAMILY.md).

<!-- modecount -->386<!-- /modecount --> modes, grouped as in the app (the list in the app also has a Plugins entry for managing add-ons and a Mode Builder entry, so [MODES.md](../MODES.md) counts 388). Every receiver is built from the published specifications and tested on generated signals (carrier offsets, noise, echoes, clock errors) and, where recordings exist, on real captures. Reports and recordings from your own antenna help most: send them on [Discord](https://discord.gg/ptxkzCuzR) or upload them to the [signal sample library](https://files.on-air.dev). Where a mode has a specific caveat it is said below. **Receive only** means nothing is transmitted, decrypted or joined.

<details>
<summary><b>All modes, by family</b></summary>

### TV

- **DVB-SH** (S band 2170 - 2200 MHz, OFDM and TDM forms), **DVB-C2** (cable, needs the cable network as input) and **ISDB-TSB** (narrow-band ISDB, one or three segments). Tested on generated signals only; DVB-T also carries the DVB-H layer (4K, MPE, time slicing, MPE-FEC).
- **DVB-T2 / DVB-T** (detected automatically; T2-Lite included): Europe, Middle East, Africa, Asia, Australia.
- **ATSC 1.0** (North America, South Korea) and **ATSC 3.0** NextGen TV (ROUTE, HEVC video with AAC audio, optional AC-4 audio through libavcodec when installed; needs 6.5 Msps or more; MMTP services are not supported).
- **ISDB-T** (Japan, Brazil and most of South America, Philippines; 13 segments, up to three layers, one-segment mobile TV).
- **DVB-S / S2 / S2X** satellite TV through an LNB, all MODCODs, VL-SNR frames, polarisation warning.
- **TV sat beacon decoder** (the narrow carrier of a geostationary beacon, found anywhere in the capture and tracked: level, C/N0, offset, drift; the reference carrier; phase-modulated telemetry recovered as bits and frames with the frame length learned from the repetition; CSV logging; a decoded view of every word of the frames with its confidence, the satellite estimated from the frequency, CSV and JSON export; [details](satbeacon.md)).
- **DTMB** (China, Hong Kong, Macau 8 MHz, Cuba 6 MHz).
- **DVB-C** cable (EN 300 429 / J.83 annex A and C, 16 to 256-QAM, symbol rate and QAM found automatically; needs a cable feed).
- **Analog TV:** PAL, SECAM, NTSC; systems B/G, I, D/K, M, N, H, K', 405-line A, C, E (819 lines), L, L'; NBTV and mechanical TV. Picture and sound.
- **FPV video (analog):** drone video links on the 5.8 GHz bands A, B, E, F, R (Raceband), L and the 1.2 GHz plan: every channel with its level, a scan that tunes to the strongest transmitter, the live picture (PAL or NTSC, frequency modulated) with its 6.0 / 6.5 MHz sound, snapshots and recording; the on-screen display is part of the picture, not decoded; needs 20 Msps ([details](fpv.md)).
- **MAC:** D2-MAC and D-MAC satellite TV (FM, 27 MHz; 20 Msps sees most of it); picture, channel name, digital sound; EuroCrypt is detected, not descrambled.
- **Internet TV (IPTV)** (no radio): playlists (M3U / M3U8) and single streams (HLS, MPEG-TS over HTTP, UDP, RTP, multicast) with channel list, favourites, XMLTV guide and the antenna TV's player; OnAir ships no channels and plays no DRM streams; [details](IPTV.md).

### Broadcast radio

- **DAB / DAB+** with a transmitter map from the TII, Journaline and MOT slide show.
- **FM** (87.5 - 108 MHz): stereo and RDS (station name, radio text, programme type, traffic flags), band scan, saved stations, Mono switch.
- **AM stereo (C-QUAM)** on medium wave: the 25 Hz pilot found, left and right from the phase of the carrier, a stereo / mono indicator; plain AM stations play as mono; the older Kahn-Hazeltine, Harris and Magnavox systems are not decoded ([details](cquam.md)).
- **DRM** (DRM30 and DRM+) on long, medium and short wave: audio, station information and the data services (slide show, website, EPG, TPEG, Journaline).
- **AM broadcast** (medium wave 531 - 1602 kHz and the shortwave broadcast bands, in the general receiver with MW and SW presets).
- **HD Radio** (NRSC-5, FM and AM) and **CDR** (China Digital Radio). HDC and DRA audio are patented or of unclear status: see [Good to know](GOOD-TO-KNOW.md). HD Radio and CDR sound play only with a codec add-on you build yourself ([docs/CODECS.md](CODECS.md)); the stations, services and data work without it.

### Aviation

- **VDL Mode 3** (receive only), **ELT 121.5 MHz** (swept-tone detection) and **military UHF air band** 225 - 400 MHz in Airband (generated signals only); ADS-B arrivals and departures board, overhead log, filter and search.
- **ADS-B** (1090 MHz): aircraft on a map with altitude, speed and callsign; aviation or metric units; feeds from other receivers; offline registrations and countries; range statistics.
- **Mode A/C** (1090 MHz secondary-radar replies): squawk and altitude of aircraft that answer radar, the ident pulse, emergency squawks, matched to the Mode S aircraft of the same capture on the ADS-B map; [details](modeac.md).
- **ASTERIX** (no radio: UDP or a recording): EUROCONTROL surveillance data, CAT 021 ADS-B, CAT 048 monoradar plots and CAT 062 system tracks, as a track table and a map; [details](asterix.md). A radar's own pulses cannot be received by a software radio, so this reads the data a radar or ground station sends.
- **UAT** (978 MHz, DO-282): ADS-B reports, FIS-B weather products, TIS-B traffic relay. **Mode S uplink** (1030 MHz, needs 8 Msps): interrogator codes, TCAS coordination.
- **ACARS**, **VDL Mode 2** (every channel at once), **VDL Mode 4** (STDMA; spec-based, no live captures validated yet) and **HFDL** (every channel in the band at once), with positions on a map.
- **Airband** (AM voice, several 25 and 8.33 kHz channels at once, scan, priority), **VOR**, **ILS** and **NDB**.
- **ADS-L** (868.2 / 868.4 / 869.525 MHz): EASA light-aviation conspicuity, traffic, status and ground uplink; hears FLARM from the same capture. Tested on synthetic signals only.
- **FLARM** (868.2 / 868.4 MHz): the old protocol is decoded; the newer one is only counted. Aircraft with the no-track or stealth flag are never shown, logged or exported.
- **DJI drones** (2.4 and 5.8 GHz, 16 Msps or more): OcuSync-like bursts are classified by shape (a guess, never an identification); DroneID is decoded. **Drone Remote ID** (ASTM F3411, BLE and Wi-Fi). Receive only.

### Maritime

- **AIS** ship positions, **VDES** (ASM decoded; VDE-TER only detected), **NAVTEX**, **DSC** and **Weather fax**.
- **Marine VHF** voice, channels 1 to 88 (156 to 162 MHz): narrow FM with squelch, a channel picker by number with names and uses, the regional plans (international, Europe, Baltic, USA, Canada, with the A / B variants), up / down, a scan with a priority watch on 16, every channel of the captured span followed at once with an activity list, a call log and a WAV per transmission. The DSC screen offers a one-click jump to the channel a call names. See [docs/marine-vhf.md](marine-vhf.md).

### Satellite and navigation

- **AX100 Mode 5 / 6, USP, NGHam, U482C, BEESAT (Mobitex)** as their own entries, **Meteosat HRDCP** and **Fleet Broadcast** (UHF SATCOM framing, nothing decoded). Generated signals only.
- **GNSS** (GPS L1; satellites, position, time and a computer-clock check).
- **SBAS** (1575.42 MHz, WAAS / EGNOS / MSAS / GAGAN / SDCM / BDSBAS): the geostationary augmentation satellites found without a GPS lock and every message decoded (PRN mask, fast corrections with UDRE, ionospheric grid, GEO navigation, time offset, almanac); the corrections are shown, not applied; QZSS L1S and L6 are not decoded; [details](sbas.md).
- **Weather pictures:** **Meteor-M LRPT** (137.1 / 137.9 MHz, Doppler tracking, channels and colour composite), **GOES HRIT** (1694.1 MHz), **Elektro-L** HRIT / LRIT and GGAK, **HRPT / AHRPT** (MetOp), **NOAA APT, HRPT, GAC and DSB** (retired satellites, for recordings; [details](SATDUMP-TIER1-3.7.md)), **Meteor HRPT**, **Elektro-L and Arktika-M RDAS** (X-band, 30 Msym/s) and **Arktika-M GGAK** (experimental), **AWS** (EUMETSAT's L-band microwave sounder link), **FY-3 AHRPT** and **FY-3 MPT** (the Fengyun-3 polar satellites: VIRR and MERSI pictures, 1701.4 / 1704.5 MHz and X-band; [details](fy3.md)). The L-band ones need a dish with an LNA at the feed.
- **Inmarsat BGAN** (signalling and carrier structure of the forward bearers, ETSI TS 102 744; a band scan over 1525 - 1559 MHz; receive only, no user data, voice or subscriber IDs; [details](BGAN-3.7.md)), **Inmarsat-C**, **Inmarsat Aero**, **Iridium** (bursts can be saved for iridium-toolkit), **Orbcomm**, **Globalstar** (activity monitor only; L-band 1610-1626.5 MHz terminal uplink, spread and encrypted; the S-band satellite downlink is not tuned), **QO-100** (beacon lock, narrowband and wideband transponders, DATV hand-over to DVB-S/S2) and **CubeSat telemetry** (AX.25 1200 / 9600 baud, AX100 modes, CCSDS Reed-Solomon, SatNOGS-style KISS out).

- **Argos** (receive only; [details](argos.md)): PTT uplink on 401.650 MHz (PTT-A2 bursts decoded to the 28 bit platform ID and the user data in hex, each platform's burst frequencies as its Doppler / drift curve; A3, ZE and Argos-4 bursts only listed, their specifications are not public) and the MetOp-B / -C downlink on 465.9875 MHz (carrier followed through the pass, measured Doppler against the pass planner's prediction, HDLC frames in hex).
- **PSK demodulator** (experimental, receive only; [details](psk-demodulator.md)): BPSK, QPSK, OQPSK and 8PSK at any symbol rate the radio holds (from about 50 Bd), found blind: symbol rate, order, pulse shape, carrier, Doppler and Eb/N0, then a constellation and eye diagram, Doppler following, DPSK / NRZ-M / NRZ-S, G3RUH and CCSDS descramblers, a sync-word hunt for the phase ambiguity and the bits saved to a file.

### Two-way radio

- **Voice inversion scrambler** (detection and descrambling, generated signals only). Single-radio trunking follows a control channel and several calls at once for Type II, EDACS and MPT 1327.
- **Tone decoders** (CTCSS, DCS, DTMF, five-tone, SELCAL and two-tone pages; [details](tones.md)).
- **DMR**, **NXDN** (NXDN48 / 96), **P25** (Phase 1, conventional and trunked), **dPMR / PMR446** (all 16 channels scanned), **D-STAR** (also in the amateur group) and **M17**: call logs with IDs and talkgroups, data and short messages. P25 encrypted calls are only reported as encrypted. Voice for the AMBE / IMBE modes needs mbelib you install yourself, see [Good to know](GOOD-TO-KNOW.md).
- **TETRA** (base station broadcast: network and colour code, location area, carriers, services, neighbours, whether encryption is used; scan over a span) and **TETRAPOL** (D_SYSTEM_INFO). Receive only, nothing decrypted; voice of unencrypted TETRA calls (TETRAPOL: clear calls) plays only through a codec plug-in you install, see [docs/CODECS.md](CODECS.md).

### Cellular and rail

- **CDMA2000 cells** and **TD-SCDMA cells** (identity broadcast only; generated signals only).
- **Cell towers** (LTE MIB / SIB1 - SIB5; towers on a map with an [OpenCelliD](TOOLS-AND-NOTES.md) key), **5G NR cells** (SS/PBCH, PCI, RSRP, MIB; SIB1 is not decoded yet), **NB-IoT cells**, **GSM-R** (921 - 925 MHz) and **LBJ** (Chinese railway train warnings at 821.2375 MHz: train number, speed, kilometre post, direction; see [docs/lbj.md](lbj.md)). Public broadcast information only.
- **KLW** end-of-train data (default 866.2375 MHz) and **TDCS** train dispatch data (default 457.550 MHz): 1200 bit/s FFSK with BCH and CRC checks, locomotive and tail IDs, pressure / commands or train number, speed, mileage and consist fields. LBJ also decodes the fixed-position extension, WGS-84 coordinates and time broadcasts. [Details and test coverage](railway.md).
- **EBICab / Eurobalise** (needs a user test, receive only: the Eurobalise balise up-link around 4.234 MHz decoded to its telegrams per UNISIG SUBSET-036; EBICab 700 / 900 telegram formats are not public and are only listed as bursts; [details](ebicab.md)).
- **Shortwave** (3.5): the General receiver (AM, USB, LSB, CW) for HF with a strip of one-click decoders: tune to a signal, press RTTY, CW, PSK31, FT8, SSTV, Olivia, ALE, NAVTEX, weather fax ... and the text or picture appears under the waterfall; the decoders follow when you retune, several run at once, **Auto** tries the likely ones, band buttons (broadcast, ham, utility, digital) remember the last frequency of each band, and there is a kHz entry box and a log per decoder. See docs/SHORTWAVE-MODE.md.
- **nRF24L01** (needs a user test, receive only: Enhanced ShockBurst frames of nRF24L01(+) chips on one 2.4 GHz channel, 250 kbit/s, 1 and 2 Mbit/s, address found by CRC; needs a 2.4 GHz SDR such as HackRF, PlutoSDR or bladeRF; [details](nrf24.md))
- **GK-2A LRIT and HRIT** (needs a user test, receive only: the Korean geostationary weather satellite at 128.2°E, LRIT 1692.14 MHz BPSK 128 ksym/s and HRIT 1695.4 MHz QPSK 3 Msym/s, two mode-list entries; frames, xRIT files, JPEG 2000 pictures per channel put together from their segments, text; the pictures are DES-encrypted by KMA, so you import your own key file; needs a dish with an LNA and GK-2A above your horizon; see docs/GK2A.md)
- **ATIS** (needs a user test, receive only: the Automatic Transmitter Identification System of the European inland waterways, not AIS; the ten digit identity a ship radio sends after each transmission on a VHF channel, 1200 baud 1300 / 2100 Hz in the DSC code, shown with country, call sign and your ship name table, with the narrow FM audio of the channel; [details](atis.md))

### Pagers

- **POCSAG** (512 - 2400 bd) and **FLEX** (1600 - 6400), whole span at once.
- **Multitone Paging**: a detector only (the format is not public); no message text is shown. Pager messages are private; receive only.

### IoT and sensors

- **Futaba S-FHSS** and **Spektrum DSM2 / DSMX** link monitors (2.4 GHz, fast SDR needed), more ISM sensor families and Wize meters. Generated signals only.
- **ISM sensors** (315 / 345 / 433.92 / 868 / 915 MHz: weather stations, tyre pressure, meters, remotes, Z-Wave; S0 and S2 stay encrypted).
- **Wireless M-Bus** (EN 13757-4 utility meters: S1, T1, C1 and R2 on 868 MHz, N1 on 169 MHz; manufacturer, ID, device type and the readings of unencrypted frames, encrypted payloads stay encrypted; [details](wmbus.md)).
- **Sigfox** (uplinks at 868.130 / 902.200 MHz and the other zones, 100 and 600 bit/s: device ID, message counter, payload hex, the three repetitions merged, CRC checked; the authentication code is never checked and nothing is decrypted; [details](sigfox.md)).
- **LoRaWAN** (EU868 / US915, all spreading factors; clear headers only, payloads stay encrypted), **Meshtastic** and **MeshCore** (nodes, map, chat, telemetry, exports), **LoRa (raw)** (plain point-to-point LoRa; hex and text frames, no decryption).
- **Bluetooth LE** (advertising, never connects), **Zigbee / 802.15.4** (2.4 GHz needs a fast radio; also the sub-GHz BPSK bands, 868.3 MHz and 906-924 MHz, with channel presets), **Thread** (the same 802.15.4 receiver: beacon network name, MAC security header, 6LoWPAN, MLE and CoAP where sent in the clear; encrypted traffic is shown as encrypted, no keys), **Wi-Fi networks** (2.4 GHz beacons, passive), **Wi-Fi HaLow** (802.11ah sub-GHz beacons), **Z-Wave** (G.9959 R1 / R2 / R3, encrypted payloads stay encrypted), **Wi-SUN FAN** (smart meter and street light mesh, all regional band plans), **ANT+ sensors** and **DECT cordless phones** (base stations from beacons; calls are encrypted, no voice; Europe 1880-1900 MHz, US/Canada DECT 6.0 1920-1930 MHz, Brazil, Latin America and the US 900 MHz ISM band, see docs/DECT.md).
- **Sub-GHz sensors** (433.92 MHz first, also 315, 868 and 915 MHz: weather stations, thermometers, tyre pressure, remotes and meters, decoded with the vendored [rtl_433](https://github.com/merbanan/rtl_433) decoders, GPL-2.0-or-later; live list with history graphs, an unknown signal analyser, custom decoders, CSV / JSON / MQTT-style export; it decodes the listed protocols only, receive only; [details](SENSORS-3.7.md)).
- **Flipper Zero Sub-GHz chat** (receive and send: chat with Flippers running the Enhanced Sub-GHz Chat app on 433.92 MHz or any frequency they use, plain or AES-256-GCM encrypted with their password or hex key; a HackRF pauses its receiver while a line goes out; [details](esubghz.md)).
- **NFC / RFID 13.56 MHz** (*experimental*): ISO/IEC 14443 Type A and Type B and ISO/IEC 15693, receive only. Tag UIDs, ATQA / SAK / ATS, CRC checks and NDEF text / URI from Type 2 tags, with a frame log and a tag table. Needs an HF-capable receiver at a few Msps so the 847.5 kHz load-modulation subcarrier fits. An active reader can transmit through the TX gate (13.553-13.567 MHz ISM band only). Nothing is cloned and no encrypted content is decoded; pet microchips are 134.2 kHz, a different band. See docs/NFC.md.

### Weather and science

- **Lightning position (network)**: strikes located from several stations' arrival times (generated signals only). Radiosonde upload to wettersonde.net and radiosondy.info, flight track as KML and GPX, launch sites.
- **Radiosondes** (400 - 406 MHz and LMS6-1680; tracks and landing prediction), **NOAA Weather Radio SAME** (EAS alert decoder).
- **Horus balloons** (Project Horus amateur high-altitude balloons on 70 cm, usually 434 MHz: Horus Binary v1 / v2 100 baud 4FSK with Golay FEC and Horus RTTY at 50 / 100 / 300 baud; every payload in the captured span on a map with its track, altitude chart, last packet, SNR, user-editable payload ID and custom field tables; works from IQ or a sound card in USB; [details](HORUS.md)).
- **SSDV pictures** (balloon and satellite pictures: 256-byte SSDV packets from FSK at 1200 - 19200 baud or RTTY-framed bytes, Reed-Solomon corrected, the JPEG built up as packets arrive and saved; code by fsphil/ssdv, GPL-3.0-or-later; [details](ssdv.md)).
- **Time signals** (DCF77, MSF, WWVB, JJY, WWV / WWVH, BPM, BPC and more; the computer clock can be synced), **e-Czas** (Poland's official time on 225 kHz long wave, from a long-wave capable radio or an upconverter; [how to receive it](eczas.md)) and **Ionospheric sounders** (ionograms from chirp sounders).
- **Radio astronomy** (21 cm hydrogen line and OH lines).
- **Sferics** (lightning: a broadband impulse counter that follows the noise floor; strikes, flashes and a per-minute histogram, mains hum trains at 100 / 120 Hz rejected; a very-low-frequency or HF capable radio; [details](SFERICS-3.7.md)).
- **GOES / Meteosat DCS** (river gauges, weather stations, tide gauges and buoys reporting on the 401.7 - 402.1 MHz uplink: every carrier in the capture followed, 100 / 300 / 1200 bps with NRZ or Manchester coding, the BCH-checked platform address, the channel and the ASCII or pseudo-binary sensor data, CSV logging; [details](goes-dcs.md)).
- **Meteor scatter** (pings of meteor trails reflecting a distant beacon such as GRAVES on 143.050 MHz: start, duration, peak level, Doppler, rates per hour and day with the showers marked, CSV logging; [details](mscatter.md)).
- **BLE tracker detector** (Apple Find My accessories such as AirTag, Tile, Chipolo, Samsung SmartTag and Google Find My Device tags recognised from their advertisements; how long and how close each stays, and an alert through the watchlists for one that follows you; receive only, identifiers kept for a short window and never saved; [details](bletrk.md)).

### Amateur: digital

- **JT6M**, **CP16** (Chinese characters on 16 carriers, receive and transmit; slow rows are not read reliably), and ham science monitors: **Schumann resonances**, **whistlers and tweeks**, **Moon echo (EME) test**, **Radio Jove**, **e-CALLISTO** solar bursts and an **aurora / Es / TEP beacon** log. Receive only apart from CP16 (which also transmits); generated signals only.
- **FSQ** and **IFKP** keyboard chat (33 tones).
- **ARDOP** and **SITOR** are listed under HF utility.
- **WSJT family:** FT8, FT4, FT2, WSPR (with an optional spot upload to wsprnet.org), JT65, JT9, Q65, FST4 / FST4W, MSK144, and **JS8Call**.
- **MT63** (500 / 1000 / 2000 Hz, short and long interleave found automatically, 64 carriers with Walsh-Hadamard coded characters; [details](mt63.md)).
- **ARDOP** (the Winlink HF modem, monitor only: frame types, connect requests, IDs and pings with call signs, ACK / NAK quality and the text of 4FSK data frames; PSK and QAM frames are listed by header; [details](ardop.md)).
- **PACTOR-I** (experimental monitor, receive only: both stations of an ARQ link, 100 / 200 baud found automatically, connect calls, control signals, ASCII and Huffman text, repeats, unproto / FEC, memory ARQ; PACTOR-II, -III and -IV are proprietary and not supported; [details](pactor.md)).
- **ROS (HF)** (detector, receive only: the ROS weak-signal mode is recognised by its tone grid and named ROS 4 / 8 / 16 baud, marked on a waterfall; the text is not decoded because ROS is not publicly specified; [details](roshf.md)).
- **Keyboard modes:** PSK31 / 63, RTTY, CW (every signal in a 14 kHz window), Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber. Picture modes: **SSTV** and **HamDRM** (EasyPal).

### Amateur: voice and packet

- **HF packet 300 Bd** (AX.25), **Winlink email** (packet, ARDOP, Telnet, or an external ARDOP / VARA / Mercury program) and an **EchoLink** client. Tested against mock servers and generated signals only.
- **Yaesu Fusion** (C4FM): callsigns, call mode and slow data; voice only through a codec plug-in you install.
- **FreeDV** (codec2 included), **M17**, **D-STAR**, **APRS (AFSK)** / AX.25 packet (whole span at once), **LoRa APRS** and **MeshCom**.

### HF utility

- **STANAG 4539**, **3G-ALE**, **Link 11 CLEW** (structure only), **STANAG 4481 FSK**, **CIS-36-50 / 50-50**, **CIS-14** and **CIS-12**; **CIS-36 MFSK, CIS OFDM HDR, CIS-128 and CIS-3000** only identify the signal. Receive only, generated signals only; nothing is decrypted.
- **HF waveform ID** (identify only): estimates the baud rate, tone spacing and repetition of a military or government HF signal and ranks candidate waveforms; nothing is decoded.
- **SITOR** (SITOR-B broadcasts and SITOR-A / AMTOR ARQ links) and **ARDOP** (Winlink HF frames, monitor only).
- **HF ALE** (2G calls, soundings, LQA), **HF beacons** (NCDXF / IARU propagation monitor), **Datawell Buoy HF Link** (wave-buoy telemetry, HXV format decoded; the 4FSK HVA format is not public) and **CODAR / HF ocean radar** (sweeps measured; propagation only, no positions).

### More 0.2.8 modes

- **eLoran / Loran-C** (100 kHz), **MPT1327**, **LTR** and **EDACS** trunking control channels (control data only, no voice), **GOES DCS** (data collection platforms, 401.7 - 402.1 MHz), **Mode A/C** (squawk and altitude), **BLE tracker detector** (AirTag, Tile and others; warns about one that follows you), **AM stereo (C-QUAM)**, **DGPS beacon** and **DME**.

### General and tools

- **General receiver:** NFM, WFM, AM, USB, LSB and CW anywhere the radio tunes, up to 4 channels from one capture, filters, squelch, recording to WAV (one file per transmission), FLAC and MP3.
- **Spectrum analyser** (1 - 6000 MHz sweeps; CSV export), **Spectrum explorer** (markers, measurements, bookmarks) and **Signal workbench** (analyse and demodulate a recording from a file, no radio needed).
</details>
