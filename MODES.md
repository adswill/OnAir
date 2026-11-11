# OnAir modes

Every mode in OnAir's mode list, grouped as in the app. Generated from `app/ui/mode_table.cpp` by `tools/gen_modes_list.py`; don't edit by hand. The quick gate fails when this file is out of date.

**316 modes.**

## TV (17)

- **DVB** - DVB-T2 and DVB-T, automatic (Europe, Middle East, Africa, Asia, Australia)
- **TV (auto)** - TV, automatic: the standard and its parameters are measured from the signal
- **ATSC** - ATSC 1.0, 8-VSB (US, Canada, Mexico, South Korea)
- **ATSC 3.0** - ATSC 3.0, NextGen TV
- **ISDB-T** - ISDB-T (Japan, Brazil, South America)
- **ISDB-TSB** - ISDB-TSB: Japanese narrow-band broadcasting of one or three ISDB-T segments (430 kHz or 1.3 MHz). Tune the centre of the band, 2 Msps
- **DVB-S/S2** - DVB-S / S2 / S2X satellite television, needs a dish and an LNB
- **Internet TV (IPTV)** - Internet TV: playlists and stream addresses (HLS, MPEG-TS, UDP, RTP), no radio needed
- **TV sat beacon decoder** - TV sat beacon decoder: carrier level, offset and drift, with telemetry frames and a decoded view when the beacon carries data
- **DTMB** - DTMB digital terrestrial television (China, Hong Kong, Macau, Cuba, Pakistan)
- **ISDB-S (BS)** - ISDB-S, BS satellite TV of Japan (ARIB STD-B20), clear streams only
- **DVB-SH** - DVB-SH: satellite and terrestrial mobile TV (S band 2170 - 2200 MHz, also UHF and L band), OFDM and TDM forms
- **DVB-C2** - DVB-C2 second generation digital cable television (OFDM), needs the cable network as the input (not an antenna)
- **DVB-C** - DVB-C digital cable television, needs the cable network as the input (not an antenna)
- **Analog TV** - Analogue television (PAL, SECAM, NTSC)
- **FPV video (analog)** - Analog FPV video (frequency modulated PAL / NTSC, 5.8 GHz bands A / B / E / F / Raceband / Low race and 1.2 GHz): every channel with its level, a scan that tunes to the strongest transmitter, the live picture with its sound subcarrier (6.0 / 6.5 MHz), snapshots and a recording as numbered pictures. The on-screen display of the flight controller is part of the picture, not decoded. Needs a radio with 20 Msps (a HackRF)
- **MAC** - MAC/packet satellite TV: picture, 4:3 / 16:9, channel name, digital sound, EuroCrypt detected

## Broadcast Radio (7)

- **DAB / DAB+** - DAB and DAB+ digital radio
- **FM** - FM broadcast radio with stereo and RDS
- **DRM** - DRM digital radio: DRM30 below 30 MHz, DRM+ in the VHF bands
- **HD Radio** - HD Radio: the digital part of hybrid FM and AM stations
- **AM broadcast** - AM broadcast: medium wave 531 - 1602 kHz and the shortwave broadcast bands, in the general receiver (AM mode)
- **AM stereo (C-QUAM)** - AM stereo: C-QUAM on medium wave, stereo found by its 25 Hz pilot
- **CDR** - CDR: China's digital radio in the FM band

## Aviation (26)

- **ADS-B** - ADS-B aircraft position and identity reports
- **Mode S uplink** - Mode S uplink: interrogators, rotation periods, aircraft asked for registers, TCAS coordination
- **Mode A/C** - Mode A/C: squawks and altitudes of aircraft answering secondary radar
- **ACARS** - ACARS: short messages between aircraft and the ground
- **VDL2** - VDL Mode 2: ACARS and ATN messages between aircraft and ground stations, positions on a map
- **VDL4** - VDL Mode 4: synchronization and DLS bursts, CPR positions when decoded in the clear
- **VDL3** - VDL Mode 3: management, voice and data bursts on a 25 kHz VHF channel, receive only
- **HFDL** - HFDL: ACARS, squitters and positions of aircraft over oceans and remote areas, on HF
- **Airband** - Airband: tower, ground and ATIS voice on several channels at once
- **VOR** - VOR: your bearing from a navigation beacon, its Morse identification and voice
- **ILS** - ILS: left / right of the runway centreline and above / below the glide path
- **NDB** - NDB: the Morse identification of a long-wave navigation beacon
- **Drone Remote ID** - Drone Remote ID: drones and their operators, from the identification every drone must broadcast
- **SELCAL (aircraft tone call)** - SELCAL: which aircraft is being called on the channel
- **UAT** - UAT 978 MHz: ADS-B, FIS-B weather and TIS-B relay
- **FANET** - FANET: paragliders and gliders over LoRa
- **OGN tracker** - OGN tracker: gliders broadcasting on 868 MHz
- **GBAS VDB** - GBAS VDB: burst structure of satellite landing data
- **LDACS (structure)** - LDACS: L-band data link, structure only
- **AeroMACS (structure)** - AeroMACS: airport data link, structure only
- **ADS-L** - ADS-L: light aircraft conspicuity on 868 and 869.5 MHz
- **FLARM** - FLARM aircraft: gliders and light aircraft on 868 MHz
- **DJI drones** - DJI drones: OcuSync / Lightbridge-like signals by shape, and DroneID packets decoded
- **DME** - DME: distance measuring equipment beacons, pulse pairs and ident
- **eLoran / Loran-C** - eLoran / Loran-C: 100 kHz hyperbolic navigation pulses, GRI and station arrival times
- **ASTERIX** - ASTERIX: radar and ADS-B surveillance data from UDP or a file

## Maritime (8)

- **AIS** - AIS: ship positions and identities
- **VDES** - VDES: ASM frames decoded, VDE-TER bursts logged and classified
- **NAVTEX** - NAVTEX: safety messages for ships on 518 and 490 kHz
- **DSC** - DSC: digital selective calling between ships and coast stations
- **Weather fax** - Weather fax: HF radiofax charts for ships
- **Marine VHF** - Marine VHF: channel picker, scan, 16 watch, whole-span activity list, recording; regional plans (ITU, Europe, Baltic, USA, Canada)
- **ATIS (inland waterways)** - ATIS: identity of the ship that just stopped talking (country, call sign, name), message list, stations heard, narrow FM audio of the channel
- **Marine radar (detect)** - Marine radar: rotation and pulse pattern

## Satellite & Navigation (48)

- **GNSS** - GNSS: satellite tracking and position fix
- **DGPS beacon** - DGPS beacons: the differential correction stations that broadcast GPS corrections
- **Inmarsat-C** - Inmarsat-C: satellite safety and news broadcasts, needs an L-band antenna
- **Inmarsat Aero** - Inmarsat Aero: aircraft satellite data link, needs an L-band antenna
- **Iridium** - Iridium satellite downlink bursts, needs an L-band antenna
- **Meteor-M LRPT** - Meteor-M LRPT: weather satellite pictures as the satellite passes
- **GOES HRIT** - GOES HRIT: geostationary weather images and EMWIN text (needs a dish and an LNA)
- **Elektro-L** - Elektro-L: geostationary weather images (HRIT / LRIT) and GGAK (needs a dish and an LNA)
- **GK-2A LRIT** - GK-2A LRIT: Korean geostationary weather pictures (needs a dish and an LNA, KMA keys for the pictures)
- **GK-2A HRIT** - GK-2A HRIT: Korean geostationary weather pictures, 3 Msym/s QPSK (needs a dish, an LNA and a fast radio)
- **FY-4 LRIT** - FY-4 LRIT: geostationary weather pictures from FengYun-4A or 4B (needs a dish and an LNA)
- **FY-4A HRIT-II/III** - FY-4A HRIT: full-resolution geostationary pictures from FengYun-4A (needs a dish and an LNA)
- **FY-2 S-VISSR** - FY-2 S-VISSR: full-disk scan-line pictures from FengYun-2 (needs a dish and an LNA)
- **COSPAS-SARSAT 406 MHz** - COSPAS-SARSAT: 406 MHz distress beacon messages decoded, with the country and position on a map (receive only; test and self-test beacons are marked)
- **Argos** - Argos: PTT uplink IDs and data at 401.65 MHz, or the 465.9875 MHz satellite downlink with its Doppler (receive only)
- **NOAA APT** - NOAA APT: 137 MHz analogue weather pictures (retired 2025, recordings)
- **NOAA DSB** - NOAA DSB: TIP telemetry and HIRS at 137 MHz (retired 2025, recordings)
- **Elektro-L RDAS** - Elektro-L RDAS: MSU-GS discs at X-band (dish, 40 Msps)
- **Arktika-M RDAS** - Arktika-M RDAS: MSU-GS discs at X-band (dish, 40 Msps)
- **Arktika-M GGAK** - Arktika-M GGAK (experimental): space-weather telemetry on 1703 MHz
- **Geoscan** - Geoscan cubesats: 9k6 FSK frames on 70 cm
- **Lucky-7** - Lucky-7 cubesat: 4k8 FSK frames
- **Spino** - Spino cubesat: 2k4 FSK frames
- **Erminaz** - Erminaz cubesat: 9k6 FSK CCSDS frames
- **Orbcomm** - Orbcomm satellite downlinks: satellite IDs, positions, channel assignments, messages
- **QO-100** - QO-100: the amateur satellite's transponders with exact frequencies from its beacons
- **CubeSat telemetry** - CubeSat telemetry: frames from amateur satellites, named and dumped
- **Globalstar** - Globalstar: channel activity and Doppler (content is not decoded)
- **GOES DCS** - GOES DCS: river gauges, weather stations and buoys reporting through the GOES and Meteosat satellites
- **Meteosat HRDCP** - Meteosat HRDCP: high rate weather and hydrology platforms reporting through Meteosat
- **Zombie satellites** - Zombie satellite carriers: found and checked against their orbits
- **What satellite is this?** - Names an unknown satellite carrier from its Doppler curve (mystery satellite hunter)
- **UHF SATCOM skimmer** - Active channels on UHF military satellites (detection only)
- **Thuraya GMR-1 channels** - Active Thuraya channels, detection only
- **Starlink detector** - Starlink frame detection, no decoding
- **OneWeb detector** - OneWeb frame detection, no decoding
- **TDRS carriers** - TDRS forward link carriers
- **Tiangong / Shenzhou carriers** - Chinese crewed spacecraft carriers
- **Tsikada / Parus** - Russian navigation satellite carriers
- **Gonets** - Gonets downlink bursts, no decoding
- **Tiantong signalling** - Tiantong activity, no decoding
- **Space station** - ISS downlinks, automatic per pass
- **NavIC** - NavIC navigation message, L5
- **SBAS** - SBAS: the correction messages of the geostationary augmentation satellites
- **LoRa satellites** - LoRa satellites: LoRa frames with the Doppler shift and its rate followed
- **PSK demodulator (experimental)** - PSK demodulator: blind symbol rate and order, constellation, bits
- **Inmarsat (all carriers)** - Inmarsat: all Aero P and Inmarsat-C carriers at once, needs an L-band antenna and a wide radio (needs a user test)
- **Inmarsat BGAN** - Inmarsat BGAN: forward carriers, bearer types, spot beams and network from the broadcast signalling (needs a user test, signalling only)

## Two-way Radio (20)

- **DMR** - DMR (Digital Mobile Radio), two-slot TDMA
- **Digital voice (auto)** - Digital voice, automatic: every system in the span found from the signal itself, with how sure
- **NXDN** - NXDN: unit IDs, talkgroups, status and short data, site information
- **P25** - P25: talkgroups, unit IDs, trunking sites and channels, voice grants (receive only; encrypted calls are only reported)
- **P25 Phase 2** - P25 Phase 2: two time slots, talkgroups, unit IDs, voice channel grants, site status (receive only; encrypted calls are only reported; AMBE+2 voice needs your own codec plug-in)
- **dPMR / PMR446** - dPMR: own and called IDs, colour codes, status and short messages, PMR446 channel scan
- **CB radio** - CB radio: channel 9 and 19, AM / FM / USB / LSB per transmission, a busy-channel list, call log and recording (receive only)
- **FRS / GMRS** - FRS / GMRS: channels 1 to 22, the CTCSS / DCS tone heard on each, call log and recording (receive only)
- **PMR446 analog** - PMR446 analog: channels 1 to 16, the CTCSS / DCS tone heard on each, call log and recording (receive only)
- **MDC-1200** - MDC-1200: unit ID, opcode and argument of each burst, with the carrier it came on (receive only; no public FleetSync format)
- **Wireless mic finder** - Wireless mic finder: candidates in the microphone bands, analog FM or not, audio of the one you pick (needs a wide radio)
- **Tone decoders (CTCSS, DCS, DTMF, selcall)** - Tone decoders: CTCSS, DCS, DTMF, ZVEI / CCIR / EEA selcall, SELCAL and two-tone pages from FM audio or a sound card
- **TETRA** - TETRA: the public broadcast information of TETRA cells
- **TETRAPOL** - TETRAPOL: system information and clear-call voice (receive only)
- **MPT1327 trunking** - MPT1327 trunking: the control channel messages of analog trunked radio systems
- **LTR trunking** - LTR trunking: the subaudible control words of an analog trunked system
- **Motorola Type II** - Motorola Type II trunking: SmartNet and SmartZone control channels and voice calls
- **TETRA DMO** - TETRA Direct Mode: DSB and DNB bursts, DM-SETUP and the calls
- **OpenSky identifier** - OpenSky: identification only, the channel is measured and nothing is decoded
- **EDACS trunking** - EDACS trunking: control channel frames, calls and analog voice following of a trunked system

## Cellular & Rail (10)

- **End of train (AAR EOT/HOT)** - End of train: US freight train telemetry
- **Cell towers** - Cell towers: the public broadcast information of LTE cells
- **5G NR cells** - 5G NR cells: the public broadcast of NR cells (PSS / SSS / PBCH, MIB)
- **NB-IoT cells** - NB-IoT cells: the public broadcast information of narrowband IoT carriers (receive only)
- **GSM-R** - GSM-R: the public broadcast information of railway GSM cells
- **LBJ** - LBJ: railway train warning broadcasts (821 MHz)
- **UMTS cells** - UMTS: 3G cells by primary scrambling code
- **CDMA2000 cells** - CDMA2000: base stations by pilot PN, SID / NID and system time from the Sync Channel
- **TD-SCDMA cells** - TD-SCDMA: cells by SYNC-DL code and cell parameter
- **EBICab / Eurobalise (needs a user test)** - Balise up-link: Eurobalise telegrams (SUBSET-036); EBICab bursts listed only (needs a user test)

## Pagers (4)

- **ERMES paging** - ERMES: European paging on 169 MHz
- **POCSAG** - POCSAG pager messages
- **FLEX** - FLEX pager messages
- **Multitone Paging** - Multitone Paging: tone and FFSK pagers for hospitals and sites, detector only (receive only)

## IoT & Sensors (38)

- **DSRC road toll (EN 12253)** - Road toll beacons: frame structure only
- **802.11p V2X (ITS-G5, DSRC)** - 802.11p: vehicle safety messages on 5.9 GHz
- **C-V2X (structure)** - C-V2X: LTE sidelink, structure only
- **ISM sensors** - ISM sensors: weather stations, TPMS, remotes and meters around you (the rtl_433 idea)
- **Sub-GHz sensors** - Sub-GHz sensors: the rtl_433 decoders with a live sensor list, history, export and an unknown signal analyser (RTL-SDR or HackRF at 250 kHz and up)
- **LoRaWAN** - LoRaWAN: IoT devices around you, from the headers every gateway receives
- **Meshtastic** - Meshtastic: LoRa mesh network nodes and messages
- **MeshCore** - MeshCore: LoRa mesh network nodes and messages
- **Bluetooth LE** - Bluetooth LE: the advertising packets devices broadcast openly
- **BLE tracker detector** - BLE tracker detector: item trackers around you, and an alert for one that follows you
- **Zigbee / 802.15.4** - Zigbee / 802.15.4: networks and devices from the headers every sniffer sees (receive only, no decryption; needs a fast 2.4 GHz radio)
- **Thread** - Thread: networks and devices from beacons and 802.15.4 headers, 6LoWPAN where sent in the clear (receive only, encrypted traffic is shown as encrypted; needs a fast 2.4 GHz radio)
- **Wi-Fi HaLow (802.11ah)** - Wi-Fi HaLow / 802.11ah: the long-range sub-GHz networks around you from their beacons (receive only, no decryption)
- **ANT+ sensors** - ANT+ sensors on 2457 MHz (and other ANT networks on their own channel): heart rate straps and watches, bike speed, cadence and power sensors, footpods, environment sensors, as a watch or bike computer hears them: 1 Mbit/s GFSK frames with the device number, type and the data page, the heart rate with the beat count and R-R interval plotted over time, power, cadence, speed, temperature, manufacturer, serial and battery pages. Receive only: nothing is sent, paired or acknowledged, and only the open broadcasts are shown. Needs a radio that reaches 2.4 GHz at 4 Msps (HackRF, Pluto, LimeSDR, BladeRF, USRP).
- **DECT cordless phones** - DECT cordless phones and base stations on the ten carriers 1881.792 - 1897.344 MHz (Europe; 1.152 Mbit/s GFSK, 10 ms frames of 24 slots): every base station from the beacon it sends every frame, with its RFPI (the manufacturer code and system number inside it), level and the slots it uses, the system information tails and a burst log with base and handset bursts. Activity and signalling only: calls are encrypted and the voice is not decoded, nothing is transmitted. Needs a radio that reaches 1.9 GHz at 5 Msps or more (HackRF, Pluto, LimeSDR, BladeRF, USRP, SDRplay).
- **Wi-SUN FAN** - Wi-SUN FAN: smart meter and street light mesh networks, nodes and links (receive only, no decryption)
- **Wireless M-Bus (meters)** - Wireless M-Bus: the utility meters around you, their IDs and readings (encrypted data stays encrypted)
- **NFC / RFID 13.56 MHz** - NFC / RFID 13.56 MHz: ISO 14443 A/B and ISO 15693 frames, tag UIDs and NDEF (receive only, nothing cloned)
- **Z-Wave** - Z-Wave: smart home networks, nodes and commands from the headers (receive only, encrypted payloads stay encrypted)
- **Bluetooth Classic** - Bluetooth Classic: devices and piconets by LAP, discoverable devices with address and class of device (receive only)
- **LoRa (raw)** - LoRa (raw): point-to-point LoRa frames as hex and text, SF / bandwidth set or searched (receive only, no decryption)
- **Sigfox** - Sigfox: uplink device IDs, counters and payloads at 868 / 902 MHz (receive only, no keys)
- **Wi-Fi networks** - Wi-Fi networks: the access points around you, from the beacons they broadcast
- **LoRaWAN (all channels)** - LoRaWAN (all channels): every channel of a regional plan at once, headers only
- **Reticulum (RNode)** - Reticulum / RNode: LoRa mesh network monitor. Tune the channel and pick bandwidth and spreading factor under Advanced, or leave the automatic search on
- **ExpressLRS** - ExpressLRS: open-source RC link. Receive only. Tune the middle of the hop plan (EU868 866.4 MHz) with a wide sample rate
- **MAVLink (SiK radio)** - MAVLink over SiK telemetry radios (ArduPilot, PX4). Receive only. A wider capture covers more hop channels
- **FrSky RC (D8 / D16)** - FrSky ACCST D8 / D16: 2.4 GHz RC link monitor, receive only. Needs a 2.4 GHz SDR
- **FlySky RC (AFHDS / 2A)** - FlySky AFHDS / AFHDS 2A: 2.4 GHz RC link monitor, receive only. Needs a 2.4 GHz SDR
- **Futaba RC (S-FHSS)** - Futaba S-FHSS: 2.4 GHz RC link monitor, receive only. Needs a 2.4 GHz SDR
- **Spektrum RC (DSM2 / DSMX)** - Spektrum DSM2 / DSMX: 2.4 GHz RC link monitor, receive only. 8 Msps or more around 2.44 GHz
- **wfb-ng / OpenHD** - wfb-ng / OpenHD: open digital video links for drones (Wi-Fi radios in monitor mode). Receive only. 20 Msps. Video stays encrypted, structure only
- **New Packet Radio (NPR-70)** - New Packet Radio (NPR-70): 4GFSK IP modems on 70 cm. Tune the channel centre, 4 Msps. Sync words and network ID only
- **WINMOR** - WINMOR: Winlink HF sound card modem, retired in 2020. Tune the dial frequency (upper sideband), any sample rate. Pilot detection only
- **mioty** - mioty: telegram-splitting LPWAN for sensors (868 MHz in Europe). Tune the channel centre (868.18 MHz), 1 Msps or more. Uplink only
- **DECT NR+** - DECT NR+ (ETSI TS 103 636): the license-exempt 1.9 GHz mesh standard. Tune the channel centre (1881.792 MHz and up in 0.864 MHz steps), 4 Msps. Control field only
- **nRF24L01 (needs a user test)** - nRF24L01: Enhanced ShockBurst frames of mice, sensors and remotes on one 2.4 GHz channel (needs a 2.4 GHz SDR)
- **Flipper Zero Sub-GHz chat** - Flipper Zero Sub-GHz chat: talk to Flippers on 433.92 MHz, plain or AES-encrypted (receive and send)

## Weather & Science (51)

- **HRPT / AHRPT** - HRPT / AHRPT: MetOp AVHRR pictures at 1.7 GHz (needs a dish and a fast radio)
- **NOAA HRPT** - NOAA HRPT: AVHRR pictures at 1.7 GHz (retired 2025, recordings)
- **NOAA GAC** - NOAA GAC: AVHRR tape dump (retired 2025, recordings)
- **Meteor HRPT** - Meteor HRPT: MSU-MR pictures at 1.7 GHz (needs a dish)
- **AWS L-band** - AWS: microwave sounder pictures on 1707 MHz (needs a dish)
- **MSG Raw Data** - MSG Raw Data: SEVIRI discs from Meteosat at 1686.83 MHz (needs a dish)
- **HimawariCast** - HimawariCast: Himawari full-disc pictures by C-band DVB-S2 (needs a dish)
- **GeoNetCast** - GeoNetCast: weather product files by C-band DVB-S2 (needs a dish and 52 Msps)
- **GOES GRB** - GOES GRB: ABI / SUVI / GLM rebroadcast (needs a dish, 12 Msps)
- **GOES GVAR** - GOES GVAR: imager broadcast (retired, recordings)
- **GOES CDA** - GOES CDA: telemetry frames at 1693 MHz (needs a dish)
- **GOES MDL** - GOES MDL: multi-use data link frames (needs a dish)
- **GK-2A UHRIT** - GK-2A UHRIT: xRIT files at X-band (dish, 28 Msps)
- **GK-2A CDAS** - GK-2A CDAS: frames (needs a dish, 8 Msps)
- **S-band CCSDS** - S-band CCSDS: satellite table, frames (needs a tracking dish)
- **JPSS HRD (NPP, NOAA-20)** - JPSS HRD (NPP, NOAA-20): X-band direct downlink, needs a dish; recordings
- **JPSS HRD (NOAA-21)** - JPSS HRD (NOAA-21): X-band direct downlink, needs a dish; recordings
- **EOS DB (Aqua, Aura)** - EOS DB (Aqua, Aura): X-band direct downlink, needs a dish; recordings
- **EOS DB (Terra)** - EOS DB (Terra): X-band direct downlink, needs a dish; recordings
- **MetOp X-band dump** - MetOp X-band dump: X-band direct downlink, needs a dish; recordings
- **MetOp-SG DDB** - MetOp-SG DDB: X-band direct downlink, needs a dish; recordings
- **Landsat 8/9 X-band** - Landsat 8/9 X-band: X-band direct downlink, needs a dish; recordings
- **Landsat 8/9 S-band** - Landsat 8/9 S-band: satellite direct downlink, needs a dish; recordings
- **Proba S-band dump** - Proba S-band dump: satellite direct downlink, needs a dish; recordings
- **Proba-V X-band dump** - Proba-V X-band dump: X-band direct downlink, needs a dish; recordings
- **GCOM-W1** - GCOM-W1: X-band direct downlink, needs a dish; recordings
- **GCOM-C1** - GCOM-C1: X-band direct downlink, needs a dish; recordings
- **STEREO high rate** - STEREO high rate: satellite direct downlink, needs a dish; recordings
- **Oceansat-2 DB** - Oceansat-2 DB: X-band direct downlink, needs a dish; recordings
- **Oceansat-3 L-band** - Oceansat-3 L-band: satellite direct downlink, needs a dish; recordings
- **Saral L-band** - Saral L-band: satellite direct downlink, needs a dish; recordings
- **Oceansat-3 X-band dump** - Oceansat-3 X-band dump: X-band direct downlink, needs a dish; recordings
- **Saral X-band dump** - Saral X-band dump: X-band direct downlink, needs a dish; recordings
- **Kanopus-V** - Kanopus-V: X-band direct downlink, needs a dish; recordings
- **DMSP RTD** - DMSP RTD: satellite direct downlink, needs a dish; recordings
- **FY-3 AHRPT** - FY-3 AHRPT: Fengyun-3 VIRR / MERSI pictures at 1.7 GHz (needs a dish and a fast radio)
- **FY-3 MPT** - FY-3 MPT: Fengyun-3 MERSI pictures in X-band (needs a dish, a down-converter and a very fast radio)
- **Weather radar (detect)** - Weather radar: scan pattern and pulse timing
- **Sferics (lightning)** - Sferics: lightning strikes counted from their radio crackle
- **Lightning position (network)** - Lightning position: strikes located from several stations' arrival times
- **SuperSID (VLF)** - SuperSID: VLF station levels and the sudden changes of solar flares, receive only
- **Riometer** - Riometer: ionospheric absorption from the cosmic noise level, receive only
- **Pulsar folding** - Pulsar folding: dedisperse and fold a record to find a pulsar, needs a dish or array
- **NOAA Weather Radio SAME** - NOAA Weather Radio SAME: weather and emergency alerts, decoded and explained (receive only)
- **Radiosonde** - Radiosonde weather balloons: position, altitude and weather data
- **Time signals** - Time signals: the time stations and your computer clock against them
- **e-Czas (225 kHz, Poland)** - e-Czas: Polish official time on 225 kHz long wave, decoded; needs a long-wave capable radio or an upconverter
- **Meteor scatter** - Meteor scatter: pings of meteor trails reflecting a distant beacon, counted and logged
- **Ionospheric sounders** - Ionospheric sounders: chirp ionosondes and the ionogram they give
- **Radio astronomy** - Radio astronomy: the hydrogen line of the Milky Way
- **SSDV pictures** - SSDV: balloon and satellite pictures, built up packet by packet

## Amateur - Digital (35)

- **QRSS grabber** - QRSS: very slow Morse read from a spectrogram, receive only
- **DFCW grabber** - DFCW: slow Morse with a frequency for dots and one for dashes, receive only
- **FSK441** - FSK441: meteor scatter pings, 441 baud 4-FSK, receive only
- **JT6M** - JT6M: 6 m meteor and ionospheric scatter, 44-tone FSK, receive only
- **ISCAT** - ISCAT: 42-tone scatter mode, A and B found automatically, receive only
- **PI4** - PI4: microwave beacon mode, 27 s frames with an 8 character message, receive only
- **Throb** - Throb: two-tone keyboard mode, six variants found automatically, receive only
- **FT8** - FT8: weak-signal amateur contacts
- **FT4** - FT4: fast weak-signal amateur contacts
- **FT2** - FT2: the fastest of the FT modes
- **WSPR** - WSPR: weak-signal propagation reports
- **JT65** - JT65: weak-signal amateur contacts and EME
- **JT9** - JT9: narrow weak-signal amateur contacts
- **Q65** - Q65: weak-signal, EME and scatter contacts
- **FST4** - FST4: slow weak-signal contacts and FST4W beacons
- **MSK144** - MSK144: meteor scatter bursts
- **JT4** - JT4: EME and weak-signal contacts
- **PSK31** - PSK31: keyboard chat, every signal in the passband
- **MT63** - MT63: robust keyboard chat on 64 carriers
- **FSQ** - FSQ: directed keyboard chat on 33 tones
- **IFKP** - IFKP: keyboard chat on 33 tones, three speeds
- **SITOR** - SITOR: the text of CCIR 476 FEC broadcasts (SITOR-B) and ARQ links (SITOR-A / AMTOR), found automatically
- **ARDOP** - ARDOP: Winlink HF modem frames, monitor only
- **JS8Call** - JS8Call: weak-signal messages, heartbeats and relays
- **RTTY** - RTTY: radio teletype text
- **CW** - CW: Morse code from every signal in the window, speed and callsigns
- **SSTV** - SSTV: slow-scan television pictures
- **HamDRM** - HamDRM: digital SSTV pictures (EasyPal, QSSTV DRM)
- **Olivia** - Olivia: text that gets through noise
- **Contestia** - Contestia: Olivia's faster cousin
- **MFSK** - MFSK: error-corrected keyboard text
- **THOR** - THOR: robust keyboard text
- **DominoEX** - DominoEX: keyboard text that ignores tuning
- **Hellschreiber** - Hell: text drawn as a picture
- **ROS (HF)** - ROS (HF): detects the ROS weak-signal mode and its sub-mode, marks it on the waterfall; text decoding partial (needs a public spec)

## Amateur - Voice & Packet (8)

- **FreeDV** - FreeDV: open digital voice on HF
- **M17** - M17: open digital voice, text messages and positions on VHF / UHF
- **D-STAR** - D-STAR: callsigns, repeaters, messages and D-PRS positions on VHF / UHF
- **Yaesu Fusion** - Yaesu Fusion: source / destination callsigns, repeater, call mode and slow data (voice only with a codec you install)
- **APRS (AFSK)** - APRS and AX.25 packet radio, 1200 bd AFSK on VHF (not LoRa APRS)
- **LoRa APRS** - LoRa APRS: APRS stations over LoRa on 433 MHz
- **MeshCom** - MeshCom: amateur LoRa mesh messages and positions on 433 MHz
- **Ham FM repeaters** - Ham FM repeaters: 2 m and 70 cm outputs with offsets, the tone heard on each, call log and recording (receive only)

## HF Utility (32)

- **PACTOR-II** - PACTOR-II: HF D-PSK carriers, symbols and frame structure (physical layer only; no text)
- **PACTOR-III** - PACTOR-III: HF multi-carrier D-PSK, carrier set and frame structure (physical layer only; no text)
- **PACTOR-IV** - PACTOR-IV: HF 1800 baud single carrier, detection and constellation estimate (physical layer only; no text)
- **MIL-STD-188-110 serial** - MIL-STD-188-110: serial tone HF data modem with STANAG 5066 framing (simulated signals only)
- **STANAG 4285** - STANAG 4285: NATO HF data modem with STANAG 5066 framing (simulated signals only)
- **STANAG 4539** - STANAG 4539: MIL-STD-188-110C high-rate HF data modem, 3200 to 12800 bit/s (simulated signals only)
- **3G-ALE** - 3G-ALE: third-generation HF link setup, traffic management and HDL / LDL data bursts BW0 to BW4 (simulated signals only)
- **Link 11 CLEW** - Link 11 CLEW: message detector and frame structure, no data (simulated signals only)
- **STANAG 4481 FSK** - STANAG 4481 FSK: naval broadcast, single or 16-channel FSK, plain text shown (simulated signals only)
- **ARQ-E** - ARQ-E: HF duplex ARQ teleprinter (simulated signals only)
- **ARQ-N** - ARQ-N: HF duplex ARQ teleprinter (simulated signals only)
- **AUTOSPEC** - AUTOSPEC: HF FEC teleprinter (simulated signals only)
- **SPREAD** - SPREAD: HF FEC teleprinter with bit spreading (simulated signals only)
- **G-TOR** - G-TOR: HF ARQ monitor (simulated signals only)
- **EFR teleswitch** - EFR: DCF49 / DCF39 radio teleswitch telegrams (simulated signals only)
- **Piccolo Mk VI** - Piccolo Mk VI: HF MFSK teleprinter (simulated signals only)
- **Coquelet-8** - Coquelet-8: HF MFSK teleprinter (simulated signals only)
- **Datawell Buoy HF Link** - Datawell Buoy HF Link: wave heights, periods and spectra from wave buoys on HF (receive only)
- **CODAR / HF ocean radar** - CODAR / HF ocean radar: the coastal radar chirps on the HF bands (receive only)
- **Shortwave** - Shortwave listening with one-click decoders: retune and they follow
- **HF ALE** - HF ALE: who calls whom, soundings and AMD messages
- **HF waveform ID** - HF waveform ID: what kind of HF data signal this is (an estimate; identification only)
- **HF beacons** - HF beacons: what is open from here, band by band
- **PACTOR-I (experimental)** - PACTOR-I: HF ARQ and FEC packets of both stations, monitor only (experimental)
- **HFGCS / HF voice** - HFGCS / HF voice: USB on the HFGCS and VOLMET frequencies, VOLMET minute schedules, a scan that steps through them (receive only)
- **CIS-36-50 / 50-50 (BEE)** - CIS-36-50 / CIS-50-50 (BEE): message framing of the Russian Navy FSK system, characters as hex (receive only, simulated only)
- **CIS-14** - CIS-14: 96 Bd FSK, 14-bit frames shown raw (receive only, simulated only)
- **CIS-12 (MS5)** - CIS-12 (MS5): 12-carrier PSK modem, pilot and phase steps (physical layer only, simulated only)
- **CIS-36 (MFSK)** - CIS-36 MFSK: identification only (simulated only)
- **CIS OFDM HDR** - CIS OFDM HDR (CIS-45 / 60 / 93): identification only (simulated only)
- **CIS-128** - CIS-128: identification only (simulated only)
- **CIS-3000** - CIS-3000: identification only (simulated only)

## General & Tools (4)

- **Signal workbench** - Signal workbench: analyse, demodulate and read any recording
- **General receiver** - General receiver: NFM, WFM, AM, USB, LSB and CW anywhere
- **Spectrum explorer** - Spectrum explorer: look, measure and sweep
- **Spectrum analyser** - Spectrum analyser: fast wide sweeps with markers and band labels

## Plugins (1)

- **Plugins** - Plugins: manage the decoder and action plugins you installed

## Kris custom modes (6)

- **KAAL** - KAAL: digital voice with a station beacon, receive and transmit
- **KAVB** - KAVB: KAAL video link, 1-2 MHz wide (no video decoder in this build)
- **KAIT** - KAIT: pictures in 10 s bursts, receive and transmit
- **KAPP** - KAPP: KAAL packet data, 100 kHz wide
- **KAFT** - KAFT: KAAL file transfer, 200 kHz to 1 MHz wide
- **KAWS** - KAWS: HF weak-signal chat and beacons, 2.4 kHz

## Custom Modes (1)

- **Mode Builder** - Mode Builder: decoders built from a description, and the custom modes you registered
