# CPU per second of signal, every mode (4.1, lane cpu2)

Baseline for the second 4.1 CPU pass. Measured by `test_qa_sweep --cpu` through `.github/workflows/cpu.yml` (method in docs/CPU-4.1.md): each mode's
built-in test signal at its default sample rate, and noise alone, through a full Engine; CPU seconds per second of signal over the steady state.
Linux = GitHub `ubuntu-24.04` (x86-64), macOS = `macos-15` (Apple silicon). Source: run 43364172327 (integ-41 2b4d103df), 393 modes, sorted by the worse of
Linux signal / noise. Figures from different runners differ (processor lottery); compare before and after only inside one run.

| # | mode | name | Msps | Linux signal | Linux noise | macOS signal | macOS noise |
|--:|---|---|--:|--:|--:|--:|--:|
| 1 | xb_landsatx | Landsat 8/9 X-band | 300 | 25.650 | 5.668 | 20.780 | 2.220 |
| 2 | xb_metopsg | MetOp-SG DDB | 140 | 15.345 | 1.944 | 15.049 | 1.413 |
| 3 | xb_oceansat3x | Oceansat-3 X-band dump | 120 | 12.302 | 1.882 | 10.893 | 1.056 |
| 4 | xb_kanopus | Kanopus-V | 90 | 8.386 | 1.378 | 6.933 | 0.920 |
| 5 | xb_gcomc | GCOM-C1 | 90 | 7.021 | 1.516 | 6.423 | 0.961 |
| 6 | fy3mpt | FY-3 MPT | 20 | 6.053 | 0.781 | 4.097 | 0.480 |
| 7 | xb_metopd | MetOp X-band dump | 45 | 6.029 | 0.707 | 5.574 | 0.461 |
| 8 | xb_probav | Proba-V X-band dump | 45 | 5.140 | 0.777 | 4.065 | 0.432 |
| 9 | geonetcast | GeoNetCast | 52 | 4.904 | 1.583 | 3.791 | 1.175 |
| 10 | xb_jpss2 | JPSS HRD (NOAA-21) | 40 | 4.070 | 0.671 | 3.639 | 0.413 |
| 11 | arktikardas | Arktika-M RDAS | 45 | 4.052 | 0.571 | 2.712 | 0.297 |
| 12 | isdbs | ISDB-S (BS) | 48 | 3.873 | 1.174 | 3.274 | 1.395 |
| 13 | elektrordas | Elektro-L RDAS | 45 | 3.375 | 0.502 | 2.856 | 0.370 |
| 14 | gk2auhrit | GK-2A UHRIT | 30 | 3.187 | 0.949 | 1.858 | 0.791 |
| 15 | xb_npp | JPSS HRD (NPP, NOAA-20) | 25 | 3.037 | 0.386 | 2.118 | 0.224 |
| 16 | xb_terra | EOS DB (Terra) | 45 | 2.897 | 0.607 | 2.076 | 0.375 |
| 17 | xb_saralx | Saral X-band dump | 25 | 2.679 | 0.338 | 2.224 | 0.193 |
| 18 | xb_oceansat2 | Oceansat-2 DB | 35 | 2.559 | 0.475 | 2.003 | 0.275 |
| 19 | dvbc2 | DVB-C2 | 10 | 1.880 | 2.526 | 1.076 | 1.646 |
| 20 | fy3ahrpt | FY-3 AHRPT | 6 | 2.022 | 0.432 | 0.814 | 0.172 |
| 21 | kavb | KAVB | 2.4 | 1.969 | 1.082 | 1.216 | 0.467 |
| 22 | xb_gcomw | GCOM-W1 | 20 | 1.863 | 0.383 | 1.189 | 0.142 |
| 23 | wisun | Wi-SUN FAN | 20 | 1.840 | 0.688 | 1.451 | 0.562 |
| 24 | dvbc | DVB-C | 10 | 1.745 | 0.217 | 1.530 | 0.188 |
| 25 | dvbsh | DVB-SH | 8 | 1.136 | 1.719 | 0.799 | 0.987 |
| 26 | elrs | ExpressLRS | 8 | 1.631 | 1.631 | 0.860 | 0.860 |
| 27 | goesgrb | GOES GRB | 20 | 1.623 | 1.059 | 1.191 | 0.881 |
| 28 | gk2ahrit | GK-2A HRIT | 10 | 0.965 | 1.565 | 0.658 | 1.383 |
| 29 | npr | New Packet Radio | 4 | 1.554 | 1.554 | 0.778 | 0.778 |
| 30 | gk2acdas | GK-2A CDAS | 8 | 1.249 | 1.407 | 0.660 | 0.746 |
| 31 | msgraw | MSG Raw Data | 6 | 0.614 | 1.383 | 0.537 | 0.790 |
| 32 | fpv | FPV video (analog) | 20 | 0.645 | 1.344 | 0.414 | 0.856 |
| 33 | dtmb | DTMB | 10 | 1.341 | 0.330 | 0.887 | 0.284 |
| 34 | xb_aqua | EOS DB (Aqua, Aura) | 15 | 1.294 | 0.285 | 1.172 | 0.162 |
| 35 | sband | S-band CCSDS | 10 | 1.236 | 1.238 | 0.728 | 0.841 |
| 36 | himawaricast | HimawariCast | 6 | 0.609 | 1.236 | 0.414 | 0.963 |
| 37 | isdbt | ISDB-T | 8 | 1.136 | 0.860 | 0.705 | 0.384 |
| 38 | halow | Wi-Fi HaLow (802.11ah) | 6 | 1.108 | 0.704 | 1.307 | 0.850 |
| 39 | dji | DJI drones | 20 | 0.976 | 0.203 | 0.592 | 0.142 |
| 40 | ism | ISM sensors | 2 | 0.969 | 0.064 | 1.019 | 0.068 |
| 41 | coquelet | Coquelet-8 | 2 | 0.960 | 0.667 | 0.531 | 0.315 |
| 42 | awsl | AWS L-band | 6 | 0.512 | 0.905 | 0.329 | 0.488 |
| 43 | goesgvar | GOES GVAR | 6 | 0.801 | 0.862 | 0.499 | 0.555 |
| 44 | atsc | ATSC 1.0 | 8 | 0.842 | 0.424 | 1.003 | 0.595 |
| 45 | dvbs | DVB-S/S2 | 10 | 0.842 | 0.309 | 0.525 | 0.227 |
| 46 | navic | NavIC | 4.096 | 0.814 | 0.243 | 0.320 | 0.128 |
| 47 | dvbt | DVB-T | 9.143 | 0.777 | 0.470 | 0.585 | 0.364 |
| 48 | rid | Drone Remote ID | 20 | 0.764 | 0.636 | 0.614 | 0.458 |
| 49 | atv | Analog TV | 10 | 0.731 | 0.293 | 0.516 | 0.207 |
| 50 | atsc3 | ATSC 3.0 | 10 | 0.701 | 0.565 | 0.396 | 0.319 |
| 51 | mac | MAC | 20 | 0.692 | 0.672 | 0.417 | 0.272 |
| 52 | bgan | Inmarsat BGAN | 2 | 0.683 | 0.007 | 0.452 | 0.009 |
| 53 | zigbee | Zigbee / 802.15.4 | 4 | 0.536 | 0.677 | 0.276 | 0.282 |
| 54 | sbas | SBAS | 4 | 0.457 | 0.660 | 0.341 | 0.474 |
| 55 | spektrum | Spektrum DSM | 8 | 0.657 | 0.657 | 0.493 | 0.493 |
| 56 | gnss | GNSS | 4 | 0.633 | 0.628 | 0.368 | 0.354 |
| 57 | meteorhrpt | Meteor HRPT | 6 | 0.506 | 0.613 | 0.429 | 0.471 |
| 58 | noaagac | NOAA GAC | 8 | 0.567 | 0.579 | 0.359 | 0.409 |
| 59 | modeac | Mode A/C | 4 | 0.574 | 0.186 | 0.337 | 0.159 |
| 60 | nfc | NFC / RFID 13.56 MHz | 4 | 0.537 | 0.574 | 0.291 | 0.389 |
| 61 | iono | Ionospheric sounders | 20 | 0.560 | 0.454 | 0.371 | 0.339 |
| 62 | fy4ahrit | FY-4A HRIT-II/III | 4 | 0.230 | 0.534 | 0.184 | 0.559 |
| 63 | thread | Thread | 4 | 0.423 | 0.529 | 0.242 | 0.294 |
| 64 | noaahrpt | NOAA HRPT | 6 | 0.408 | 0.512 | 0.283 | 0.375 |
| 65 | cv2x | C-V2X | 15.36 | 0.500 | 0.500 | 0.253 | 0.253 |
| 66 | vdl2 | VDL Mode 2 | 2 | 0.482 | 0.480 | 0.301 | 0.278 |
| 67 | hrdcp | Meteosat HRDCP | 1 | 0.472 | 0.032 | 0.097 | 0.021 |
| 68 | t2 | DVB-T2 | 9.143 | 0.468 | 0.117 | 0.375 | 0.105 |
| 69 | wifi | Wi-Fi networks | 20 | 0.455 | 0.446 | 0.393 | 0.371 |
| 70 | specan | Spectrum analyser | 20 | 0.407 | 0.427 | 0.232 | 0.230 |
| 71 | kaft | KAFT | 2.4 | 0.416 | 0.406 | 0.441 | 0.365 |
| 72 | dect | DECT cordless phones | 5 | 0.299 | 0.397 | 0.245 | 0.288 |
| 73 | sfhss | Futaba S-FHSS | 8 | 0.393 | 0.393 | 0.258 | 0.258 |
| 74 | frsky | FrSky | 8 | 0.388 | 0.388 | 0.370 | 0.370 |
| 75 | qo100 | QO-100 | 10 | 0.385 | 0.320 | 0.407 | 0.398 |
| 76 | dectnrp | DECT NR+ | 4 | 0.385 | 0.385 | 0.267 | 0.267 |
| 77 | xb_landsats | Landsat 8/9 S-band | 6 | 0.380 | 0.108 | 0.255 | 0.067 |
| 78 | hrpt | HRPT / AHRPT | 6 | 0.370 | 0.378 | 0.278 | 0.249 |
| 79 | papr | Crest factor (PAPR / CCDF) meter | 1 | 0.368 | 0.368 | 0.347 | 0.347 |
| 80 | xb_probas | Proba S-band dump | 4 | 0.349 | 0.060 | 0.277 | 0.028 |
| 81 | fm | FM | 4 | 0.316 | 0.347 | 0.172 | 0.167 |
| 82 | kapp | KAPP | 0.25 | 0.321 | 0.346 | 0.256 | 0.290 |
| 83 | ebicab | EBICab / Eurobalise | 10 | 0.321 | 0.345 | 0.333 | 0.295 |
| 84 | mavlink | MAVLink (SiK) | 8 | 0.339 | 0.339 | 0.251 | 0.251 |
| 85 | wfbng | wfb-ng / OpenHD | 20 | 0.337 | 0.337 | 0.245 | 0.245 |
| 86 | flysky | FlySky | 8 | 0.332 | 0.332 | 0.281 | 0.281 |
| 87 | pskd | PSK demodulator | 2 | 0.039 | 0.329 | 0.036 | 0.214 |
| 88 | cell | Cell towers | 20 | 0.325 | 0.282 | 0.302 | 0.329 |
| 89 | xlink | Extra links | 20 | 0.324 | 0.324 | 0.282 | 0.282 |
| 90 | its | 802.11p V2X | 10 | 0.316 | 0.316 | 0.153 | 0.153 |
| 91 | isdbtsb | ISDB-TSB | 2.048 | 0.311 | 0.150 | 0.169 | 0.075 |
| 92 | nrf24 | nRF24L01 | 8 | 0.311 | 0.166 | 0.175 | 0.115 |
| 93 | goes | GOES HRIT | 2.4 | 0.208 | 0.305 | 0.172 | 0.333 |
| 94 | lorawanwide | LoRaWAN (all channels) | 4 | 0.304 | 0.287 | 0.209 | 0.239 |
| 95 | btc | Bluetooth Classic | 20 | 0.303 | 0.294 | 0.267 | 0.276 |
| 96 | nr | 5G NR cells | 20 | 0.292 | 0.295 | 0.351 | 0.365 |
| 97 | inmsat | Inmarsat (all carriers) | 6 | 0.277 | 0.053 | 0.141 | 0.048 |
| 98 | oneweb | OneWeb detector | 20 | 0.273 | 0.274 | 0.186 | 0.186 |
| 99 | hfdl | HFDL | 2 | 0.245 | 0.267 | 0.152 | 0.161 |
| 100 | iridium | Iridium | 10 | 0.259 | 0.137 | 0.119 | 0.092 |
| 101 | wmic | Wireless mic finder | 10 | 0.253 | 0.133 | 0.179 | 0.103 |
| 102 | xb_oceansat3 | Oceansat-3 L-band | 3 | 0.252 | 0.040 | 0.194 | 0.023 |
| 103 | elektro | Elektro-L | 2.4 | 0.197 | 0.249 | 0.179 | 0.240 |
| 104 | ant | ANT+ sensors | 4 | 0.218 | 0.247 | 0.142 | 0.183 |
| 105 | dab | DAB | 10 | 0.245 | 0.142 | 0.184 | 0.142 |
| 106 | adsl | ADS-L | 2 | 0.229 | 0.225 | 0.241 | 0.227 |
| 107 | goesmdl | GOES MDL | 3 | 0.211 | 0.228 | 0.124 | 0.134 |
| 108 | esubghz | Flipper Zero Sub-GHz chat | 2 | 0.226 | 0.221 | 0.044 | 0.052 |
| 109 | xb_stereo | STEREO high rate | 3 | 0.219 | 0.040 | 0.148 | 0.019 |
| 110 | rclink | RC and mesh links | 8 | 0.205 | 0.205 | 0.109 | 0.109 |
| 111 | fy2svissr | FY-2 S-VISSR | 2.4 | 0.172 | 0.205 | 0.170 | 0.138 |
| 112 | adsb | ADS-B | 4 | 0.201 | 0.136 | 0.122 | 0.086 |
| 113 | gmr1 | Thuraya GMR-1 channels | 10 | 0.201 | 0.201 | 0.082 | 0.090 |
| 114 | lorawan | LoRaWAN | 2 | 0.194 | 0.199 | 0.161 | 0.120 |
| 115 | starlink | Starlink detector | 20 | 0.197 | 0.196 | 0.224 | 0.242 |
| 116 | burst | Burst finder | 2 | 0.192 | 0.192 | 0.156 | 0.156 |
| 117 | fy4lrit | FY-4 LRIT | 2 | 0.112 | 0.191 | 0.074 | 0.128 |
| 118 | glitch | SDR stream glitch detector | 2 | 0.189 | 0.189 | 0.115 | 0.115 |
| 119 | kaal | KAAL | 0.24 | 0.189 | 0.169 | 0.119 | 0.109 |
| 120 | hop | Frequency-hopper analyser | 2 | 0.186 | 0.186 | 0.176 | 0.176 |
| 121 | uhfsatcom | UHF SATCOM skimmer | 10 | 0.181 | 0.184 | 0.101 | 0.124 |
| 122 | xb_saral | Saral L-band | 3 | 0.181 | 0.039 | 0.119 | 0.021 |
| 123 | general | General receiver | 2 | 0.173 | 0.175 | 0.053 | 0.051 |
| 124 | am | AM broadcast | 2 | 0.173 | 0.174 | 0.036 | 0.039 |
| 125 | aeromacs | AeroMACS | 5.6 | 0.172 | 0.172 | 0.134 | 0.134 |
| 126 | kait | KAIT | 1 | 0.147 | 0.172 | 0.049 | 0.070 |
| 127 | airband | Airband | 2 | 0.171 | 0.163 | 0.125 | 0.114 |
| 128 | hdr | HD Radio | 2 | 0.060 | 0.169 | 0.069 | 0.155 |
| 129 | reticulum | Reticulum | 8 | 0.167 | 0.167 | 0.119 | 0.119 |
| 130 | tiantong | Tiantong signalling | 10 | 0.161 | 0.163 | 0.133 | 0.126 |
| 131 | stanag4539 | STANAG 4539 / 110C high rate | 2 | 0.079 | 0.162 | 0.033 | 0.053 |
| 132 | eloran | eLoran / Loran-C | 0.5 | 0.160 | 0.160 | 0.117 | 0.117 |
| 133 | cdr | CDR | 2 | 0.040 | 0.160 | 0.025 | 0.064 |
| 134 | specx | Spectrum explorer | 10 | 0.145 | 0.155 | 0.118 | 0.123 |
| 135 | bletracker | BLE tracker detector | 4 | 0.137 | 0.155 | 0.092 | 0.085 |
| 136 | ble | Bluetooth LE | 4 | 0.137 | 0.154 | 0.106 | 0.077 |
| 137 | shortwave | Shortwave | 2 | 0.146 | 0.145 | 0.058 | 0.062 |
| 138 | radar | Radar pulse analyser | 2 | 0.142 | 0.142 | 0.113 | 0.113 |
| 139 | marinevhf | Marine VHF | 8 | 0.141 | 0.141 | 0.072 | 0.072 |
| 140 | noisefloor | Noise floor logger | 2 | 0.141 | 0.141 | 0.080 | 0.080 |
| 141 | uplink | Mode S uplink | 10 | 0.119 | 0.140 | 0.080 | 0.073 |
| 142 | ssdv | SSDV pictures | 2 | 0.115 | 0.136 | 0.110 | 0.119 |
| 143 | birdie | Persistent carrier (birdie) finder | 2 | 0.135 | 0.135 | 0.073 | 0.073 |
| 144 | intermod | Intermodulation finder | 2 | 0.133 | 0.133 | 0.072 | 0.072 |
| 145 | globalstar | Globalstar | 10 | 0.131 | 0.130 | 0.049 | 0.047 |
| 146 | hfbeacon | HF beacons | 10 | 0.128 | 0.125 | 0.056 | 0.057 |
| 147 | wradar | Weather radar | 10 | 0.128 | 0.128 | 0.114 | 0.114 |
| 148 | xb_dmsp | DMSP RTD | 2 | 0.124 | 0.025 | 0.092 | 0.011 |
| 149 | satbeacon | TV sat beacon decoder | 2 | 0.120 | 0.047 | 0.130 | 0.032 |
| 150 | spur | Spur and image finder | 2 | 0.119 | 0.119 | 0.056 | 0.056 |
| 151 | occupancy | Spectrum occupancy monitor | 2 | 0.116 | 0.116 | 0.053 | 0.053 |
| 152 | mradar | Marine radar | 10 | 0.113 | 0.113 | 0.096 | 0.096 |
| 153 | loraraw | LoRa (raw) | 2 | 0.111 | 0.106 | 0.066 | 0.075 |
| 154 | riometer | Riometer | 2 | 0.104 | 0.105 | 0.046 | 0.069 |
| 155 | vdes | VDES | 1 | 0.104 | 0.028 | 0.079 | 0.021 |
| 156 | codar | CODAR / HF ocean radar | 2 | 0.092 | 0.104 | 0.043 | 0.044 |
| 157 | pulsar | Pulsar folding | 2.4 | 0.100 | 0.097 | 0.039 | 0.047 |
| 158 | yfactor | Noise figure (Y-factor) meter | 2 | 0.098 | 0.098 | 0.112 | 0.112 |
| 159 | vdl4 | VDL Mode 4 | 2 | 0.095 | 0.093 | 0.080 | 0.101 |
| 160 | inmc | Inmarsat-C | 2 | 0.087 | 0.093 | 0.036 | 0.047 |
| 161 | ogn | OGN tracker | 1 | 0.091 | 0.091 | 0.047 | 0.047 |
| 162 | wmbus | Wireless M-Bus (meters) | 2 | 0.069 | 0.090 | 0.061 | 0.106 |
| 163 | sarsat | COSPAS-SARSAT 406 MHz | 0.256 | 0.089 | 0.052 | 0.074 | 0.028 |
| 164 | dsrc | DSRC toll | 4 | 0.083 | 0.083 | 0.070 | 0.070 |
| 165 | gonets | Gonets | 4 | 0.083 | 0.083 | 0.034 | 0.033 |
| 166 | uhfrfid | UHF RFID reader (EPC Gen2) | 2 | 0.082 | 0.082 | 0.083 | 0.083 |
| 167 | m17 | M17 | 2 | 0.072 | 0.082 | 0.059 | 0.088 |
| 168 | flarm | FLARM | 1 | 0.081 | 0.081 | 0.057 | 0.055 |
| 169 | gk2alrit | GK-2A LRIT | 2.4 | 0.069 | 0.081 | 0.080 | 0.078 |
| 170 | usp | USP (Sputnix) | 2 | 0.080 | 0.081 | 0.054 | 0.062 |
| 171 | umts | UMTS cells | 7.68 | 0.080 | 0.080 | 0.041 | 0.052 |
| 172 | u482c | U482C (GomX-1) | 2 | 0.077 | 0.078 | 0.042 | 0.041 |
| 173 | ax100m5 | AX100 Mode 5 (ASM + Golay) | 2 | 0.076 | 0.077 | 0.053 | 0.047 |
| 174 | vdl3 | VDL Mode 3 | 1 | 0.071 | 0.071 | 0.041 | 0.048 |
| 175 | callisto | e-CALLISTO bursts | 2 | 0.070 | 0.069 | 0.036 | 0.036 |
| 176 | ale3g | 3G-ALE | 2 | 0.054 | 0.070 | 0.049 | 0.072 |
| 177 | radiojove | Radio Jove | 2 | 0.069 | 0.069 | 0.047 | 0.040 |
| 178 | gsmr | GSM-R | 5 | 0.068 | 0.068 | 0.104 | 0.091 |
| 179 | ngham | NGHam | 2 | 0.067 | 0.063 | 0.040 | 0.042 |
| 180 | ax100m6 | AX100 Mode 6 (Reed-Solomon) | 2 | 0.064 | 0.067 | 0.041 | 0.046 |
| 181 | vor | VOR | 2 | 0.066 | 0.053 | 0.038 | 0.030 |
| 182 | cubesat | CubeSat telemetry | 2 | 0.061 | 0.066 | 0.041 | 0.049 |
| 183 | nxdn | NXDN | 2 | 0.065 | 0.065 | 0.030 | 0.042 |
| 184 | evm | Modulation quality (MER) meter | 0.25 | 0.065 | 0.065 | 0.036 | 0.036 |
| 185 | tdrs | TDRS carriers | 2 | 0.064 | 0.065 | 0.043 | 0.056 |
| 186 | beesat | BEESAT / Mobitex | 2 | 0.064 | 0.065 | 0.046 | 0.048 |
| 187 | dstar | D-STAR | 2 | 0.050 | 0.064 | 0.048 | 0.060 |
| 188 | ils | ILS | 2 | 0.063 | 0.047 | 0.049 | 0.037 |
| 189 | occbw | Occupied bandwidth meter | 1 | 0.063 | 0.063 | 0.029 | 0.029 |
| 190 | meteor | Meteor-M LRPT | 1 | 0.052 | 0.063 | 0.038 | 0.044 |
| 191 | autospec | AUTOSPEC | 2 | 0.062 | 0.039 | 0.034 | 0.031 |
| 192 | hfdig | HF digital | 2 | 0.059 | 0.062 | 0.059 | 0.059 |
| 193 | sonde | Radiosonde | 8 | 0.058 | 0.062 | 0.043 | 0.044 |
| 194 | uat | UAT (978 MHz) | 2 | 0.035 | 0.062 | 0.027 | 0.028 |
| 195 | aero | Inmarsat Aero | 4 | 0.060 | 0.020 | 0.051 | 0.029 |
| 196 | mioty | mioty | 4 | 0.059 | 0.059 | 0.041 | 0.041 |
| 197 | cishdr | CIS OFDM HDR | 2 | 0.058 | 0.029 | 0.051 | 0.020 |
| 198 | cdma2000 | CDMA2000 cells | 2.458 | 0.052 | 0.058 | 0.025 | 0.044 |
| 199 | vorils | VOR / ILS | 2 | 0.057 | 0.044 | 0.041 | 0.042 |
| 200 | mesh | Mesh (LoRa) | 2 | 0.057 | 0.053 | 0.043 | 0.037 |
| 201 | cb | CB radio | 1 | 0.057 | 0.013 | 0.042 | 0.007 |
| 202 | cquam | AM stereo (C-QUAM) | 2 | 0.056 | 0.053 | 0.043 | 0.043 |
| 203 | marine | Marine | 2 | 0.055 | 0.051 | 0.042 | 0.036 |
| 204 | dominoex | DominoEX | 2 | 0.055 | 0.051 | 0.039 | 0.035 |
| 205 | freedv | FreeDV | 2 | 0.055 | 0.052 | 0.047 | 0.037 |
| 206 | link11 | Link 11 CLEW | 2 | 0.054 | 0.037 | 0.067 | 0.035 |
| 207 | goesdcs | GOES DCS | 1 | 0.054 | 0.032 | 0.054 | 0.028 |
| 208 | pager | Pagers | 2 | 0.044 | 0.054 | 0.043 | 0.040 |
| 209 | spreadhf | SPREAD | 2 | 0.053 | 0.034 | 0.033 | 0.027 |
| 210 | pocsag | POCSAG | 2 | 0.045 | 0.053 | 0.041 | 0.046 |
| 211 | ais | AIS | 2 | 0.052 | 0.020 | 0.037 | 0.014 |
| 212 | meshcore | MeshCore | 2 | 0.051 | 0.048 | 0.029 | 0.022 |
| 213 | mfsk | MFSK | 2 | 0.049 | 0.051 | 0.038 | 0.040 |
| 214 | cis128 | CIS-128 | 2 | 0.050 | 0.028 | 0.046 | 0.016 |
| 215 | arqe | ARQ-E | 2 | 0.050 | 0.045 | 0.038 | 0.045 |
| 216 | dpmr | dPMR | 2 | 0.049 | 0.050 | 0.048 | 0.053 |
| 217 | jt65 | JT65 | 2 | 0.047 | 0.050 | 0.037 | 0.032 |
| 218 | flex | FLEX | 2 | 0.042 | 0.050 | 0.024 | 0.029 |
| 219 | ft4 | FT4 | 2 | 0.049 | 0.047 | 0.039 | 0.047 |
| 220 | acars | ACARS | 2 | 0.049 | 0.015 | 0.045 | 0.011 |
| 221 | pactor4 | PACTOR-IV | 2 | 0.043 | 0.049 | 0.045 | 0.052 |
| 222 | ssrint | SSR interrogations (1030 MHz) | 4 | 0.048 | 0.048 | 0.035 | 0.035 |
| 223 | hamfm | Ham FM repeaters | 2 | 0.048 | 0.022 | 0.035 | 0.013 |
| 224 | olivia | Olivia | 2 | 0.047 | 0.048 | 0.042 | 0.037 |
| 225 | frs | FRS / GMRS | 2 | 0.047 | 0.023 | 0.061 | 0.016 |
| 226 | pactor2 | PACTOR-II | 2 | 0.044 | 0.047 | 0.055 | 0.041 |
| 227 | msk144 | MSK144 | 2 | 0.047 | 0.047 | 0.031 | 0.032 |
| 228 | q65 | Q65 | 2 | 0.047 | 0.046 | 0.040 | 0.037 |
| 229 | wspr | WSPR | 2 | 0.047 | 0.047 | 0.028 | 0.037 |
| 230 | ft2 | FT2 | 2 | 0.047 | 0.047 | 0.039 | 0.042 |
| 231 | p25p2 | P25 Phase 2 | 2 | 0.046 | 0.046 | 0.037 | 0.037 |
| 232 | cw | CW | 2 | 0.046 | 0.032 | 0.024 | 0.030 |
| 233 | rtty | RTTY | 2 | 0.046 | 0.046 | 0.028 | 0.029 |
| 234 | packet | APRS / Packet | 2 | 0.045 | 0.040 | 0.033 | 0.036 |
| 235 | ldacs | LDACS | 2 | 0.045 | 0.045 | 0.028 | 0.028 |
| 236 | issdown | Space station | 1 | 0.044 | 0.043 | 0.034 | 0.032 |
| 237 | whatsat | What satellite is this? | 1 | 0.043 | 0.044 | 0.032 | 0.042 |
| 238 | stanag4481 | STANAG 4481 FSK | 2 | 0.041 | 0.044 | 0.029 | 0.035 |
| 239 | astro | Radio astronomy | 2.5 | 0.041 | 0.044 | 0.039 | 0.035 |
| 240 | drm | DRM | 2 | 0.042 | 0.043 | 0.044 | 0.038 |
| 241 | zwave | Z-Wave | 2 | 0.043 | 0.025 | 0.027 | 0.016 |
| 242 | arqn | ARQ-N | 2 | 0.043 | 0.034 | 0.021 | 0.074 |
| 243 | css | Tiangong / Shenzhou carriers | 1 | 0.043 | 0.043 | 0.028 | 0.031 |
| 244 | thor | THOR | 2 | 0.043 | 0.041 | 0.030 | 0.030 |
| 245 | jt9 | JT9 | 2 | 0.039 | 0.043 | 0.035 | 0.029 |
| 246 | speckurt | Spectral kurtosis RFI finder | 2 | 0.042 | 0.042 | 0.055 | 0.055 |
| 247 | hamdrm | HamDRM | 2 | 0.042 | 0.042 | 0.040 | 0.039 |
| 248 | ft8 | FT8 | 2 | 0.042 | 0.040 | 0.034 | 0.033 |
| 249 | ndb | NDB | 0.256 | 0.042 | 0.013 | 0.036 | 0.012 |
| 250 | hell | Hellschreiber | 2 | 0.040 | 0.042 | 0.035 | 0.037 |
| 251 | jt4 | JT4 | 2 | 0.039 | 0.041 | 0.042 | 0.035 |
| 252 | ale | HF ALE | 2 | 0.041 | 0.037 | 0.036 | 0.040 |
| 253 | multitone | Multitone Paging | 2 | 0.041 | 0.025 | 0.037 | 0.018 |
| 254 | dme | DME | 2.4 | 0.041 | 0.041 | 0.039 | 0.039 |
| 255 | contestia | Contestia | 2 | 0.041 | 0.040 | 0.029 | 0.040 |
| 256 | fst4 | FST4 | 2 | 0.040 | 0.041 | 0.035 | 0.033 |
| 257 | atis | ATIS (inland waterways) | 1 | 0.039 | 0.041 | 0.028 | 0.028 |
| 258 | p25 | P25 | 2 | 0.038 | 0.041 | 0.038 | 0.036 |
| 259 | pactor3 | PACTOR-III | 2 | 0.040 | 0.038 | 0.046 | 0.050 |
| 260 | sstv | SSTV | 2 | 0.040 | 0.040 | 0.037 | 0.037 |
| 261 | pmr446 | PMR446 analog | 1 | 0.040 | 0.015 | 0.028 | 0.009 |
| 262 | lorasat | LoRa satellites | 2 | 0.038 | 0.040 | 0.038 | 0.029 |
| 263 | meshtastic | Meshtastic | 2 | 0.039 | 0.029 | 0.018 | 0.016 |
| 264 | opensky | OpenSky identifier | 0.24 | 0.039 | 0.039 | 0.029 | 0.029 |
| 265 | gtor | G-TOR | 2 | 0.032 | 0.039 | 0.025 | 0.025 |
| 266 | goescda | GOES CDA | 1 | 0.038 | 0.038 | 0.029 | 0.032 |
| 267 | dsc | DSC | 2 | 0.037 | 0.038 | 0.021 | 0.025 |
| 268 | piccolo | Piccolo Mk VI | 2 | 0.036 | 0.038 | 0.026 | 0.031 |
| 269 | psk | PSK31 | 2 | 0.033 | 0.038 | 0.023 | 0.021 |
| 270 | wefax | Weather fax | 2 | 0.032 | 0.038 | 0.024 | 0.033 |
| 271 | ardop | ARDOP | 2 | 0.036 | 0.037 | 0.041 | 0.035 |
| 272 | js8 | JS8Call | 2 | 0.037 | 0.028 | 0.019 | 0.020 |
| 273 | dmr | DMR | 2.4 | 0.037 | 0.037 | 0.028 | 0.033 |
| 274 | milhf | HF waveform ID | 2 | 0.037 | 0.037 | 0.030 | 0.030 |
| 275 | tdcs | TDCS | 0.96 | 0.036 | 0.037 | 0.028 | 0.031 |
| 276 | zombie | Zombie satellites | 1 | 0.036 | 0.037 | 0.026 | 0.030 |
| 277 | meshcom | MeshCom | 2 | 0.035 | 0.037 | 0.020 | 0.022 |
| 278 | milstd110 | MIL-STD-188-110 serial | 2 | 0.032 | 0.037 | 0.021 | 0.022 |
| 279 | mdc1200 | MDC-1200 | 2 | 0.036 | 0.021 | 0.036 | 0.017 |
| 280 | fusion | Yaesu Fusion | 2 | 0.036 | 0.036 | 0.031 | 0.031 |
| 281 | cisbee | CIS-36-50 (BEE) | 2 | 0.036 | 0.029 | 0.029 | 0.021 |
| 282 | voiceinv | Voice inversion scrambler | 2 | 0.036 | 0.033 | 0.020 | 0.019 |
| 283 | orbcomm | Orbcomm | 1 | 0.035 | 0.010 | 0.036 | 0.036 |
| 284 | sigfox | Sigfox | 1.024 | 0.035 | 0.025 | 0.033 | 0.024 |
| 285 | mt63 | MT63 | 2 | 0.035 | 0.032 | 0.031 | 0.028 |
| 286 | cp16 | CP16 | 2 | 0.035 | 0.034 | 0.030 | 0.021 |
| 287 | tsikada | Tsikada / Parus | 1 | 0.034 | 0.035 | 0.029 | 0.030 |
| 288 | jove | Radio Jove bursts | 1 | 0.034 | 0.034 | 0.038 | 0.038 |
| 289 | time | Time signals | 0.256 | 0.034 | 0.034 | 0.011 | 0.012 |
| 290 | sitor | SITOR | 2 | 0.034 | 0.034 | 0.018 | 0.026 |
| 291 | phasenoise | Phase noise meter | 0.25 | 0.034 | 0.034 | 0.020 | 0.020 |
| 292 | efr | EFR teleswitch | 2 | 0.026 | 0.034 | 0.022 | 0.028 |
| 293 | gnssjam | GNSS interference detector | 4 | 0.033 | 0.033 | 0.020 | 0.020 |
| 294 | tetradmo | TETRA DMO | 0.24 | 0.033 | 0.033 | 0.033 | 0.033 |
| 295 | navtex | NAVTEX | 2 | 0.029 | 0.033 | 0.023 | 0.016 |
| 296 | ifkp | IFKP | 2 | 0.032 | 0.032 | 0.028 | 0.030 |
| 297 | tdscdma | TD-SCDMA cells | 2.56 | 0.032 | 0.028 | 0.026 | 0.028 |
| 298 | mscatter | Meteor scatter | 2 | 0.032 | 0.032 | 0.023 | 0.024 |
| 299 | cis3000 | CIS-3000 | 2 | 0.032 | 0.029 | 0.032 | 0.025 |
| 300 | fsq | FSQ | 2 | 0.028 | 0.032 | 0.019 | 0.025 |
| 301 | uvb76 | UVB-76 buzzer monitor | 1 | 0.031 | 0.031 | 0.041 | 0.041 |
| 302 | klw | KLW | 0.96 | 0.030 | 0.031 | 0.026 | 0.024 |
| 303 | cis12 | CIS-12 | 2 | 0.030 | 0.031 | 0.017 | 0.021 |
| 304 | pactor | PACTOR-I | 2 | 0.030 | 0.028 | 0.033 | 0.032 |
| 305 | stanag4285 | STANAG 4285 | 2 | 0.030 | 0.027 | 0.028 | 0.021 |
| 306 | tones | Tone decoders (CTCSS, DCS, DTMF, selcall) | 2 | 0.029 | 0.030 | 0.023 | 0.019 |
| 307 | fskan | FSK parameter analyser | 0.25 | 0.029 | 0.029 | 0.034 | 0.034 |
| 308 | nbiot | NB-IoT cells | 2 | 0.029 | 0.029 | 0.025 | 0.025 |
| 309 | same | NOAA Weather Radio SAME | 2 | 0.029 | 0.026 | 0.020 | 0.023 |
| 310 | roshf | ROS (HF) | 2 | 0.028 | 0.029 | 0.021 | 0.020 |
| 311 | hfpacket300 | HF packet 300 Bd | 2 | 0.027 | 0.029 | 0.023 | 0.020 |
| 312 | cis36 | CIS-36 | 2 | 0.027 | 0.028 | 0.025 | 0.022 |
| 313 | cis14 | CIS-14 | 2 | 0.027 | 0.028 | 0.019 | 0.018 |
| 314 | elt121 | ELT 121.5 MHz | 2 | 0.026 | 0.027 | 0.031 | 0.022 |
| 315 | fmmon | FM modulation monitor | 0.25 | 0.027 | 0.027 | 0.027 | 0.027 |
| 316 | tetra | TETRA | 2 | 0.027 | 0.024 | 0.022 | 0.012 |
| 317 | loraaprs | LoRa APRS | 2 | 0.027 | 0.023 | 0.022 | 0.020 |
| 318 | fsk441 | FSK441 | 0.6 | 0.026 | 0.025 | 0.013 | 0.011 |
| 319 | ookpulse | OOK / ASK pulse analyser | 0.25 | 0.026 | 0.026 | 0.025 | 0.025 |
| 320 | kaws | KAWS | 2 | 0.026 | 0.025 | 0.025 | 0.018 |
| 321 | smartnet | Motorola Type II trunking | 0.96 | 0.025 | 0.025 | 0.034 | 0.034 |
| 322 | selcal | SELCAL (aircraft tone call) | 2 | 0.022 | 0.025 | 0.024 | 0.028 |
| 323 | noaaapt | NOAA APT | 1.024 | 0.024 | 0.024 | 0.026 | 0.027 |
| 324 | noaadsb | NOAA DSB | 1.024 | 0.024 | 0.024 | 0.025 | 0.029 |
| 325 | winmor | WINMOR | 4 | 0.024 | 0.024 | 0.024 | 0.024 |
| 326 | ammon | AM modulation monitor | 0.25 | 0.024 | 0.024 | 0.008 | 0.008 |
| 327 | whistler | Whistlers and tweeks | 0.048 | 0.024 | 0.023 | 0.006 | 0.006 |
| 328 | pi4 | PI4 | 0.6 | 0.023 | 0.022 | 0.014 | 0.017 |
| 329 | qrss | QRSS grabber | 0.6 | 0.023 | 0.023 | 0.012 | 0.011 |
| 330 | rcppm | RC model PPM (72 MHz) | 1 | 0.022 | 0.022 | 0.021 | 0.021 |
| 331 | argos | Argos | 0.256 | 0.021 | 0.006 | 0.006 | 0.024 |
| 332 | fanet | FANET | 1 | 0.021 | 0.021 | 0.017 | 0.017 |
| 333 | dfcw | DFCW grabber | 0.6 | 0.020 | 0.019 | 0.011 | 0.011 |
| 334 | jt6m | JT6M | 0.6 | 0.019 | 0.020 | 0.013 | 0.013 |
| 335 | lbj | LBJ | 0.96 | 0.018 | 0.020 | 0.012 | 0.011 |
| 336 | sferics | Sferics (lightning) | 2 | 0.017 | 0.019 | 0.028 | 0.036 |
| 337 | iscat | ISCAT | 0.6 | 0.019 | 0.019 | 0.013 | 0.012 |
| 338 | throb | Throb | 0.6 | 0.019 | 0.019 | 0.013 | 0.011 |
| 339 | dgps | DGPS beacon | 0.96 | 0.017 | 0.017 | 0.018 | 0.018 |
| 340 | schumann | Schumann resonances | 0.048 | 0.017 | 0.017 | 0.009 | 0.014 |
| 341 | inputhealth | SDR input health meter | 2 | 0.017 | 0.017 | 0.009 | 0.009 |
| 342 | arktikaggak | Arktika-M GGAK | 1.024 | 0.016 | 0.016 | 0.017 | 0.020 |
| 343 | xlinknb | Extra links, narrow | 4 | 0.015 | 0.015 | 0.033 | 0.033 |
| 344 | eot | End of train | 1 | 0.015 | 0.015 | 0.009 | 0.009 |
| 345 | horus | Horus balloons | 0.96 | 0.013 | 0.013 | 0.012 | 0.015 |
| 346 | dcr | DCR (Japan digital convenience radio) | 0.25 | 0.013 | 0.013 | 0.010 | 0.010 |
| 347 | ermes | ERMES | 1 | 0.012 | 0.012 | 0.032 | 0.032 |
| 348 | passband | Passband signal classifier | 0.096 | 0.012 | 0.012 | 0.013 | 0.013 |
| 349 | ltpos | Lightning position (network) | 1 | 0.011 | 0.010 | 0.011 | 0.011 |
| 350 | hfvoice | HFGCS / HF voice | 0.25 | 0.011 | 0.005 | 0.006 | 0.023 |
| 351 | geoscan | Geoscan | 0.25 | 0.011 | 0.011 | 0.011 | 0.010 |
| 352 | wb | Signal workbench | 2 | 0.009 | 0.009 | 0.038 | 0.038 |
| 353 | asterix | ASTERIX | 2 | 0.007 | 0.009 | 0.033 | 0.034 |
| 354 | supersid | SuperSID (VLF) | 0.25 | 0.009 | 0.008 | 0.006 | 0.010 |
| 355 | erminaz | Erminaz | 0.25 | 0.009 | 0.009 | 0.008 | 0.008 |
| 356 | edacs | EDACS trunking | 0.48 | 0.008 | 0.008 | 0.030 | 0.030 |
| 357 | iptv | Internet TV (IPTV) | 2 | 0.007 | 0.008 | 0.015 | 0.047 |
| 358 | lucky7 | Lucky-7 | 0.25 | 0.007 | 0.006 | 0.033 | 0.028 |
| 359 | tetrapol | TETRAPOL | 0.25 | 0.007 | 0.007 | 0.023 | 0.004 |
| 360 | nfmdev | Narrowband FM deviation and CTCSS meter | 0.048 | 0.007 | 0.007 | 0.018 | 0.018 |
| 361 | qi | Qi wireless charging | 0.25 | 0.007 | 0.007 | 0.010 | 0.010 |
| 362 | em4100 | EM4100 LF RFID (125 kHz) | 0.25 | 0.007 | 0.007 | 0.007 | 0.007 |
| 363 | spino | Spino | 0.25 | 0.006 | 0.006 | 0.030 | 0.007 |
| 364 | mpt1327 | MPT1327 trunking | 0.24 | 0.006 | 0.006 | 0.016 | 0.016 |
| 365 | graves | GRAVES meteor echoes | 0.25 | 0.006 | 0.006 | 0.007 | 0.007 |
| 366 | beacon | Beacon propagation monitor | 0.25 | 0.006 | 0.006 | 0.007 | 0.007 |
| 367 | sensors | Sub-GHz sensors | 0.25 | 0.006 | 0.005 | 0.006 | 0.006 |
| 368 | crowd36 | CROWD36 | 0.048 | 0.005 | 0.005 | 0.021 | 0.021 |
| 369 | xpa | XPA (10 / 20 Bd) | 0.048 | 0.005 | 0.005 | 0.020 | 0.020 |
| 370 | ltr | LTR trunking | 0.48 | 0.005 | 0.005 | 0.004 | 0.004 |
| 371 | fbr | Fleet Broadcast (UHF SATCOM) | 0.25 | 0.004 | 0.005 | 0.002 | 0.029 |
| 372 | datawell | Datawell Buoy HF Link | 0.25 | 0.004 | 0.003 | 0.025 | 0.032 |
| 373 | dopplerlog | Satellite Doppler curve logger | 0.048 | 0.004 | 0.004 | 0.018 | 0.018 |
| 374 | xpa2 | XPA2 | 0.048 | 0.004 | 0.004 | 0.005 | 0.005 |
| 375 | plnoise | Power-line noise locator | 0.25 | 0.004 | 0.004 | 0.004 | 0.004 |
| 376 | gbas | GBAS VDB | 0.25 | 0.004 | 0.004 | 0.004 | 0.004 |
| 377 | eczas | e-Czas (225 kHz, Poland) | 0.256 | 0.003 | 0.003 | 0.025 | 0.019 |
| 378 | f01 | FSK200/500 (F01) | 0.048 | 0.003 | 0.003 | 0.019 | 0.019 |
| 379 | sinad | FM SINAD meter | 0.048 | 0.003 | 0.003 | 0.015 | 0.015 |
| 380 | ssbtest | SSB transmitter test meter | 0.048 | 0.003 | 0.003 | 0.014 | 0.014 |
| 381 | f06 | FSK200/1000 (F06, F06a) | 0.048 | 0.003 | 0.003 | 0.013 | 0.013 |
| 382 | afsk | AFSK (TNC) tone tuner | 0.048 | 0.003 | 0.003 | 0.013 | 0.013 |
| 383 | gwfsk | GW FSK (100 Bd) | 0.048 | 0.003 | 0.003 | 0.012 | 0.012 |
| 384 | fskmodem | FSK modems (Bell 103, V.21, V.23, Bell 202) | 0.048 | 0.003 | 0.003 | 0.003 | 0.003 |
| 385 | emeecho | Moon echo (EME) | 0.25 | 0.002 | 0.002 | 0.026 | 0.022 |
| 386 | marker | ILS marker beacons (75 MHz) | 0.048 | 0.002 | 0.002 | 0.016 | 0.016 |
| 387 | vlfmsk | VLF MSK transmitter monitor | 0.048 | 0.002 | 0.002 | 0.014 | 0.014 |
| 388 | cwkeying | CW keying quality meter | 0.048 | 0.002 | 0.002 | 0.010 | 0.010 |
| 389 | beaconevents | Aurora / Es / TEP beacons | 0.25 | 0.001 | 0.001 | 0.023 | 0.022 |
| 390 | txstability | Transmitter frequency stability meter | 0.048 | 0.001 | 0.001 | 0.019 | 0.019 |
| 391 | fading | Fading analyser | 0.048 | 0.001 | 0.001 | 0.015 | 0.015 |
| 392 | pttlog | PTT and duty-cycle logger | 0.048 | 0.001 | 0.001 | 0.011 | 0.011 |
| 393 | freqacc | Carrier frequency accuracy meter | 0.048 | 0.001 | 0.001 | 0.010 | 0.010 |
