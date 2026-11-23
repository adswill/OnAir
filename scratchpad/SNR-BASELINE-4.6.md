# SNR threshold baseline (4.6 step 1)

Test: `snr_threshold` (tests/qa/test_snr_threshold.cpp, label `snr;slow`, not in fast ci.yml). It drives tools/sens/qa_sens.py over
test_qa_sweep --gen/--run and writes `snr-out/snr-threshold.csv` (mode,name,kind,threshold_dB,notes).

SNR = active signal power / white Gaussian noise power inside the signal's own 99 % band (channel bandwidth, not Eb/N0). Decoded = checked
block count >= half the clean run's, in every one of 2 fixed-seed trials. Threshold = lowest SNR reached from +20 dB down (4 dB steps, 2 dB near the edge, floor -20) with no failing point.
Empty = no decode. Wideband TV modes and a few others are skipped by qa_sens.py SKIP_DEFAULT (they have sens_sweep).

Run (20 modes first, then all):
    gh workflow run quick.yml -R adswill/OnAir --ref integ-46-snr -f tests=snr_threshold -f env="SNR_LIMIT=20" -f timeout=7200 -f minutes=120
    gh workflow run quick.yml -R adswill/OnAir --ref integ-46-snr -f tests=snr_threshold -f timeout=21600 -f minutes=350
Env (-f env=): SNR_LIMIT first N modes, SNR_RE mode-id regex, SNR_JOBS (3), SNR_CAP seconds per mode (240, then no more points), SNR_OUT.
Output: log lines `SNRTABLE mode,threshold_dB,notes` (worst first), `SNRWORST` (10 worst above 0 dB), artifact `snr-threshold` (CSV + jsonl).

## Baseline run 45029190841 (full, 377 modes, branch integ-46-snr)

153 modes above 0 dB, 123 with no decode (empty threshold: no valid clean output or no pass up to +44 dB), 15 FALSE_DECODE (threshold not trusted).

Worst 10 above 0 dB (trusted): multitone 24, pmr446 24, frs 24, sonde 22, hdr 22, tetra 22, hamfm 22, mdc1200 22, elektrordas 20, arktikardas 20

FALSE_DECODE (count decodes on noise alone): nfc, noaagac, meteorhrpt, noaadsb, specan, goesmdl, radiojove, callisto, shortwave, noaaapt, astro, supersid, schumann, emeecho, beaconevents

Cause for noaagac, noaadsb, meteorhrpt: core/src/wxsat/wxsat_rx.cpp gac()/dsb()/meteorCadu() call good() (blocksOk++) on every frame the weak sync detector delivers, with no CRC/RS check (weak check, not a counter bug). noaahrpt validates the frame, so it is clean.

| mode | name | kind | threshold_dB | notes |
|---|---|---|---|---|
| multitone | Multitone Paging | blocks | 24 |  |
| pmr446 | PMR446 analog | blocks | 24 |  |
| frs | FRS / GMRS | blocks | 24 |  |
| sonde | Radiosonde | blocks | 22 |  |
| hdr | HD Radio | blocks | 22 |  |
| tetra | TETRA | blocks | 22 |  |
| hamfm | Ham FM repeaters | blocks | 22 |  |
| mdc1200 | MDC-1200 | blocks | 22 |  |
| elektrordas | Elektro-L RDAS | blocks | 20 | runtime cap hit |
| arktikardas | Arktika-M RDAS | blocks | 20 | runtime cap hit |
| fy3ahrpt | FY-3 AHRPT | blocks | 20 | runtime cap hit |
| fy3mpt | FY-3 MPT | blocks | 20 | runtime cap hit |
| gonets | Gonets | blocks | 20 |  |
| fm | FM | blocks | 18 |  |
| modeac | Mode A/C | blocks | 18 |  |
| nfc | NFC / RFID 13.56 MHz | blocks | 18 | FALSE_DECODE: 798 units in 2 noise-only trials, threshold not trusted |
| gk2auhrit | GK-2A UHRIT | blocks | 18 | runtime cap hit |
| isdbtsb | ISDB-TSB | blocks | 18 |  |
| drm | DRM | blocks | 16 |  |
| gnss | GNSS | blocks | 16 | runtime cap hit |
| msgraw | MSG Raw Data | blocks | 16 | runtime cap hit |
| halow | Wi-Fi HaLow (802.11ah) | blocks | 16 | runtime cap hit |
| ant | ANT+ sensors | blocks | 16 | runtime cap hit |
| eczas | e-Czas (225 kHz, Poland) | blocks | 16 |  |
| kavb | KAVB | blocks | 16 | runtime cap hit |
| kait | KAIT | blocks | 16 | runtime cap hit |
| cb | CB radio | blocks | 16 |  |
| lucky7 | Lucky-7 | blocks | 16 |  |
| gk2acdas | GK-2A CDAS | blocks | 16 | runtime cap hit |
| coquelet | Coquelet-8 | blocks | 16 | runtime cap hit |
| xb_npp | JPSS HRD (NPP, NOAA-20) | blocks | 16 | runtime cap hit |
| xb_jpss2 | JPSS HRD (NOAA-21) | blocks | 16 | runtime cap hit |
| xb_metopd | MetOp X-band dump | blocks | 16 | runtime cap hit |
| xb_metopsg | MetOp-SG DDB | blocks | 16 | runtime cap hit |
| xb_landsatx | Landsat 8/9 X-band | blocks | 16 | runtime cap hit |
| xb_probav | Proba-V X-band dump | blocks | 16 | runtime cap hit |
| xb_saralx | Saral X-band dump | blocks | 16 | runtime cap hit |
| xb_kanopus | Kanopus-V | blocks | 16 | runtime cap hit |
| starlink | Starlink detector | blocks | 16 | runtime cap hit |
| ils | ILS | blocks | 16 |  |
| adsb | ADS-B | blocks | 14 |  |
| vdl4 | VDL Mode 4 | blocks | 14 |  |
| kaal | KAAL | blocks | 14 | runtime cap hit |
| geoscan | Geoscan | blocks | 14 |  |
| gtor | G-TOR | blocks | 14 |  |
| vdl3 | VDL Mode 3 | blocks | 14 |  |
| ax100m6 | AX100 Mode 6 (Reed-Solomon) | blocks | 14 |  |
| u482c | U482C (GomX-1) | blocks | 14 |  |
| hamdrm | HamDRM | saved | 14 |  |
| mfsk | MFSK | blocks | 14 |  |
| geonetcast | GeoNetCast | blocks | 12 | runtime cap hit |
| zigbee | Zigbee / 802.15.4 | blocks | 12 | runtime cap hit |
| thread | Thread | blocks | 12 | runtime cap hit |
| sbas | SBAS | blocks | 12 | runtime cap hit |
| esubghz | Flipper Zero Sub-GHz chat | blocks | 12 |  |
| ssdv | SSDV pictures | blocks | 12 |  |
| atis | ATIS (inland waterways) | blocks | 12 |  |
| selcal | SELCAL (aircraft tone call) | blocks | 12 |  |
| hfvoice | HFGCS / HF voice | blocks | 12 |  |
| goesgrb | GOES GRB | blocks | 12 | runtime cap hit |
| goesgvar | GOES GVAR | blocks | 12 | runtime cap hit |
| spino | Spino | blocks | 12 |  |
| erminaz | Erminaz | blocks | 12 |  |
| sband | S-band CCSDS | blocks | 12 | runtime cap hit |
| xb_aqua | EOS DB (Aqua, Aura) | blocks | 12 | runtime cap hit |
| xb_terra | EOS DB (Terra) | blocks | 12 | runtime cap hit |
| xb_gcomw | GCOM-W1 | blocks | 12 | runtime cap hit |
| xb_gcomc | GCOM-C1 | blocks | 12 | runtime cap hit |
| xb_saral | Saral L-band | blocks | 12 | runtime cap hit |
| xb_oceansat2 | Oceansat-2 DB | blocks | 12 | runtime cap hit |
| xb_oceansat3x | Oceansat-3 X-band dump | blocks | 12 | runtime cap hit |
| cis3000 | CIS-3000 | lock | 12 | lock only |
| packet | APRS / Packet | blocks | 10 |  |
| ism | ISM sensors | blocks | 10 |  |
| cubesat | CubeSat telemetry | blocks | 10 |  |
| tetrapol | TETRAPOL | blocks | 10 |  |
| tones | Tone decoders (CTCSS, DCS, DTMF, selcall) | blocks | 10 |  |
| cquam | AM stereo (C-QUAM) | blocks | 10 |  |
| ebicab | EBICab / Eurobalise | blocks | 10 |  |
| nrf24 | nRF24L01 | blocks | 10 |  |
| sensors | Sub-GHz sensors | blocks | 10 |  |
| ngham | NGHam | blocks | 10 |  |
| beesat | BEESAT / Mobitex | blocks | 10 |  |
| voiceinv | Voice inversion scrambler | blocks | 10 |  |
| ais | AIS | blocks | 8 |  |
| noaahrpt | NOAA HRPT | blocks | 8 | runtime cap hit |
| noaagac | NOAA GAC | blocks | 8 | runtime cap hit; FALSE_DECODE: 192 units in 2 noise-only trials, threshold not trusted |
| arktikaggak | Arktika-M GGAK | blocks | 8 |  |
| meteorhrpt | Meteor HRPT | blocks | 8 | runtime cap hit; FALSE_DECODE: 119 units in 2 noise-only trials, threshold not trusted |
| awsl | AWS L-band | blocks | 8 | runtime cap hit |
| himawaricast | HimawariCast | blocks | 8 | runtime cap hit |
| dstar | D-STAR | blocks | 8 |  |
| nxdn | NXDN | blocks | 8 |  |
| dpmr | dPMR | blocks | 8 |  |
| uplink | Mode S uplink | blocks | 8 | runtime cap hit |
| mac | MAC | blocks | 8 | runtime cap hit |
| p25 | P25 | blocks | 8 |  |
| general | General receiver | blocks | 8 |  |
| dect | DECT cordless phones | blocks | 8 | runtime cap hit |
| klw | KLW | blocks | 8 |  |
| tdcs | TDCS | blocks | 8 |  |
| gk2ahrit | GK-2A HRIT | blocks | 8 | runtime cap hit |
| uat | UAT (978 MHz) | blocks | 8 |  |
| bgan | Inmarsat BGAN | blocks | 8 | runtime cap hit |
| xb_landsats | Landsat 8/9 S-band | blocks | 8 | runtime cap hit |
| xb_oceansat3 | Oceansat-3 L-band | blocks | 8 | runtime cap hit |
| ax100m5 | AX100 Mode 5 (ASM + Golay) | blocks | 8 |  |
| usp | USP (Sputnix) | blocks | 8 |  |
| hfpacket300 | HF packet 300 Bd | blocks | 8 |  |
| freedv | FreeDV | blocks | 8 |  |
| am | AM broadcast | blocks | 8 |  |
| dmr | DMR | blocks | 6 |  |
| noaadsb | NOAA DSB | blocks | 6 | FALSE_DECODE: 77 units in 6 noise-only trials, threshold not trusted |
| cdr | CDR | blocks | 6 |  |
| same | NOAA Weather Radio SAME | blocks | 6 |  |
| ndb | NDB | blocks | 6 |  |
| bletracker | BLE tracker detector | blocks | 6 |  |
| rpr | Robust Packet (RPR) | blocks | 6 |  |
| fy4ahrit | FY-4A HRIT-II/III | blocks | 6 |  |
| stanag4285 | STANAG 4285 | blocks | 6 |  |
| cis12 | CIS-12 | blocks | 6 |  |
| cis128 | CIS-128 | lock | 6 | lock only |
| elt121 | ELT 121.5 MHz | blocks | 6 |  |
| link11 | Link 11 CLEW | blocks | 6 |  |
| vdl2 | VDL Mode 2 | blocks | 4 | runtime cap hit |
| vorils | VOR / ILS | blocks | 4 |  |
| cw | CW | blocks | 4 |  |
| m17 | M17 | blocks | 4 |  |
| specan | Spectrum analyser | blocks | 4 | runtime cap hit; FALSE_DECODE: 78124 units in 2 noise-only trials, threshold not trusted |
| ble | Bluetooth LE | blocks | 4 |  |
| rid | Drone Remote ID | blocks | 4 | runtime cap hit |
| espnow | ESP-NOW | blocks | 4 | runtime cap hit |
| hrpt | HRPT / AHRPT | blocks | 4 | runtime cap hit |
| lbj | LBJ | blocks | 4 |  |
| wisun | Wi-SUN FAN | blocks | 4 | runtime cap hit |
| kapp | KAPP | blocks | 4 | runtime cap hit |
| kaft | KAFT | blocks | 4 | runtime cap hit |
| hrdcp | Meteosat HRDCP | blocks | 4 |  |
| lorawanwide | LoRaWAN (all channels) | blocks | 4 | runtime cap hit |
| roshf | ROS (HF) | blocks | 4 | runtime cap hit |
| inmsat | Inmarsat (all carriers) | blocks | 4 | runtime cap hit |
| pskd | PSK demodulator | blocks | 4 |  |
| ltpos | Lightning position (network) | lock | 4 | lock only |
| goescda | GOES CDA | blocks | 4 |  |
| wmic | Wireless mic finder | lock | 4 | runtime cap hit; lock only |
| goesmdl | GOES MDL | blocks | 4 | runtime cap hit; FALSE_DECODE: 440 units in 4 noise-only trials, threshold not trusted |
| xb_probas | Proba S-band dump | blocks | 4 | runtime cap hit |
| cishdr | CIS OFDM HDR | lock | 4 | lock only |
| sstv | SSTV | blocks | 4 |  |
| vor | VOR | blocks | 4 |  |
| wefax | Weather fax | lock | 4 | runtime cap hit; lock only |
| pager | Pagers | blocks | 2 |  |
| meteor | Meteor-M LRPT | blocks | 2 |  |
| flarm | FLARM | blocks | 2 |  |
| wmbus | Wireless M-Bus (meters) | blocks | 2 |  |
| milstd110 | MIL-STD-188-110 serial | blocks | 2 |  |
| efr | EFR teleswitch | blocks | 2 |  |
| fbr | Fleet Broadcast (UHF SATCOM) | blocks | 2 |  |
| flex | FLEX | blocks | 2 |  |
| inmc | Inmarsat-C | blocks | 0 | runtime cap hit |
| orbcomm | Orbcomm | blocks | 0 |  |
| iono | Ionospheric sounders | lock | 0 | runtime cap hit; lock only |
| qo100 | QO-100 | lock | 0 | runtime cap hit; lock only |
| time | Time signals | lock | 0 | lock only |
| goes | GOES HRIT | blocks | 0 | runtime cap hit |
| elektro | Elektro-L | blocks | 0 | runtime cap hit |
| fy4lrit | FY-4 LRIT | blocks | 0 |  |
| fy2svissr | FY-2 S-VISSR | blocks | 0 | runtime cap hit |
| btc | Bluetooth Classic | blocks | 0 | runtime cap hit |
| arqe | ARQ-E | blocks | 0 | runtime cap hit |
| arqn | ARQ-N | blocks | 0 | runtime cap hit |
| radiojove | Radio Jove | lock | 0 | runtime cap hit; lock only; FALSE_DECODE: 4 units in 4 noise-only trials, threshold not trusted |
| callisto | e-CALLISTO bursts | lock | 0 | runtime cap hit; lock only; FALSE_DECODE: 4 units in 4 noise-only trials, threshold not trusted |
| xb_stereo | STEREO high rate | blocks | 0 | runtime cap hit |
| uhfsatcom | UHF SATCOM skimmer | blocks | 0 | runtime cap hit |
| gmr1 | Thuraya GMR-1 channels | blocks | 0 | runtime cap hit |
| oneweb | OneWeb detector | blocks | 0 | runtime cap hit |
| tiantong | Tiantong signalling | blocks | 0 | runtime cap hit |
| cis14 | CIS-14 | blocks | 0 | runtime cap hit |
| stanag4539 | STANAG 4539 / 110C high rate | blocks | 0 |  |
| ft2 | FT2 | blocks | 0 | runtime cap hit |
| pocsag | POCSAG | blocks | 0 |  |
| lorawan | LoRaWAN | blocks | 0 | runtime cap hit |
| gk2alrit | GK-2A LRIT | blocks | -2 |  |
| pactor4 | PACTOR-IV | lock | -2 | lock only |
| sigfox | Sigfox | blocks | -2 |  |
| dsc | DSC | blocks | -2 |  |
| iridium | Iridium | blocks | -4 | runtime cap hit |
| airband | Airband | blocks | -4 | runtime cap hit |
| specx | Spectrum explorer | lock | -4 | runtime cap hit; lock only |
| globalstar | Globalstar | blocks | -4 | runtime cap hit |
| datawell | Datawell Buoy HF Link | blocks | -4 |  |
| horus | Horus balloons | blocks | -4 |  |
| sarsat | COSPAS-SARSAT 406 MHz | blocks | -4 |  |
| sitor | SITOR | blocks | -4 |  |
| argos | Argos | blocks | -4 |  |
| adsl | ADS-L | blocks | -4 | runtime cap hit |
| whistler | Whistlers and tweeks | blocks | -4 |  |
| cisbee | CIS-36-50 (BEE) | blocks | -4 | runtime cap hit |
| cis36 | CIS-36 | lock | -4 | runtime cap hit; lock only |
| stanag4481 | STANAG 4481 FSK | blocks | -4 |  |
| hell | Hellschreiber | blocks | -4 |  |
| acars | ACARS | blocks | -6 |  |
| ale | HF ALE | blocks | -6 |  |
| vdes | VDES | blocks | -6 |  |
| codar | CODAR / HF ocean radar | blocks | -6 |  |
| zwave | Z-Wave | blocks | -6 |  |
| satbeacon | TV sat beacon decoder | blocks | -6 |  |
| sferics | Sferics (lightning) | blocks | -6 |  |
| aero | Inmarsat Aero | blocks | -8 | runtime cap hit |
| hfdig | HF digital | blocks | -8 |  |
| hfdl | HFDL | blocks | -8 | runtime cap hit |
| shortwave | Shortwave | lock | -8 | runtime cap hit; lock only; FALSE_DECODE: 8 units in 8 noise-only trials, threshold not trusted |
| throb | Throb | blocks | -8 | runtime cap hit |
| xb_dmsp | DMSP RTD | blocks | -8 | runtime cap hit |
| tdrs | TDRS carriers | lock | -8 | lock only |
| rtty | RTTY | blocks | -8 |  |
| thor | THOR | blocks | -8 |  |
| dominoex | DominoEX | blocks | -8 |  |
| iscat | ISCAT | blocks | -10 |  |
| piccolo | Piccolo Mk VI | blocks | -10 |  |
| ale3g | 3G-ALE | blocks | -10 |  |
| contestia | Contestia | blocks | -10 |  |
| loraraw | LoRa (raw) | blocks | -12 | runtime cap hit |
| cp16 | CP16 | blocks | -12 |  |
| meshcore | MeshCore | blocks | -12 |  |
| olivia | Olivia | blocks | -12 |  |
| mscatter | Meteor scatter | lock | -14 | lock only |
| mesh | Mesh (LoRa) | blocks | -16 | runtime cap hit |
| pi4 | PI4 | blocks | -16 | runtime cap hit |
| pactor | PACTOR-I | blocks | -18 |  |
| noaaapt | NOAA APT | blocks | -20 | FALSE_DECODE: 193 units in 14 noise-only trials, threshold not trusted |
| psk | PSK31 | blocks | -20 |  |
| astro | Radio astronomy | lock | -20 | lock only; FALSE_DECODE: 14 units in 14 noise-only trials, threshold not trusted |
| mt63 | MT63 | blocks | -20 |  |
| fsq | FSQ | blocks | -20 |  |
| ifkp | IFKP | blocks | -20 |  |
| ardop | ARDOP | blocks | -20 |  |
| goesdcs | GOES DCS | blocks | -20 |  |
| lorasat | LoRa satellites | blocks | -20 |  |
| fsk441 | FSK441 | blocks | -20 |  |
| supersid | SuperSID (VLF) | lock | -20 | lock only; FALSE_DECODE: 14 units in 14 noise-only trials, threshold not trusted |
| schumann | Schumann resonances | lock | -20 | lock only; FALSE_DECODE: 14 units in 14 noise-only trials, threshold not trusted |
| jt6m | JT6M | blocks | -20 |  |
| emeecho | Moon echo (EME) | lock | -20 | lock only; FALSE_DECODE: 14 units in 14 noise-only trials, threshold not trusted |
| beaconevents | Aurora / Es / TEP beacons | lock | -20 | lock only; FALSE_DECODE: 14 units in 14 noise-only trials, threshold not trusted |
| zombie | Zombie satellites | lock | -20 | lock only |
| whatsat | What satellite is this? | lock | -20 | lock only |
| css | Tiangong / Shenzhou carriers | lock | -20 | lock only |
| tsikada | Tsikada / Parus | lock | -20 | lock only |
| issdown | Space station | lock | -20 | lock only |
| meshtastic | Meshtastic | blocks | -20 |  |
| loraaprs | LoRa APRS | blocks | -20 |  |
| meshcom | MeshCom | blocks | -20 |  |
| marine | Marine |  | none | no valid output on the clean test signal (8 and 32 s) |
| rclink | RC and mesh links |  | none | no test signal or generator failed |
| gsmr | GSM-R |  | none | no valid output on the clean test signal (8 and 32 s) |
| js8 | JS8Call | blocks | none | no 90 % point up to 44 dB |
| nbiot | NB-IoT cells |  | none | no valid output on the clean test signal (8 and 32 s) |
| hfbeacon | HF beacons |  | none | no valid output on the clean test signal (8 and 32 s) |
| wb | Signal workbench |  | none | no test signal or generator failed |
| dgps | DGPS beacon |  | none | no test signal or generator failed |
| dme | DME |  | none | no test signal or generator failed |
| eloran | eLoran / Loran-C |  | none | no test signal or generator failed |
| mpt1327 | MPT1327 trunking |  | none | no test signal or generator failed |
| fusion | Yaesu Fusion |  | none | no test signal or generator failed |
| kaws | KAWS |  | none | no valid output on the clean test signal (8 and 32 s) |
| milhf | HF waveform ID |  | none | no test signal or generator failed |
| asterix | ASTERIX |  | none | no valid output on the clean test signal (8 and 32 s) |
| ltr | LTR trunking |  | none | no test signal or generator failed |
| edacs | EDACS trunking |  | none | no test signal or generator failed |
| pactor2 | PACTOR-II |  | none | no valid output on the clean test signal (8 and 32 s) |
| pactor3 | PACTOR-III |  | none | no valid output on the clean test signal (8 and 32 s) |
| p25p2 | P25 Phase 2 |  | none | no test signal or generator failed |
| marinevhf | Marine VHF |  | none | no test signal or generator failed |
| smartnet | Motorola Type II trunking |  | none | no test signal or generator failed |
| tetradmo | TETRA DMO |  | none | no test signal or generator failed |
| opensky | OpenSky identifier |  | none | no test signal or generator failed |
| fanet | FANET |  | none | no test signal or generator failed |
| ogn | OGN tracker |  | none | no test signal or generator failed |
| gbas | GBAS VDB |  | none | no test signal or generator failed |
| ldacs | LDACS |  | none | no test signal or generator failed |
| aeromacs | AeroMACS |  | none | no test signal or generator failed |
| eot | End of train |  | none | no test signal or generator failed |
| dsrc | DSRC toll |  | none | no test signal or generator failed |
| its | 802.11p V2X |  | none | no test signal or generator failed |
| cv2x | C-V2X |  | none | no test signal or generator failed |
| ermes | ERMES |  | none | no test signal or generator failed |
| mradar | Marine radar |  | none | no test signal or generator failed |
| wradar | Weather radar |  | none | no test signal or generator failed |
| rcppm | RC model PPM (72 MHz) |  | none | no test signal or generator failed |
| uvb76 | UVB-76 buzzer monitor |  | none | no test signal or generator failed |
| graves | GRAVES meteor echoes |  | none | no test signal or generator failed |
| jove | Radio Jove bursts |  | none | no test signal or generator failed |
| beacon | Beacon propagation monitor |  | none | no test signal or generator failed |
| em4100 | EM4100 LF RFID (125 kHz) |  | none | no test signal or generator failed |
| uhfrfid | UHF RFID reader (EPC Gen2) |  | none | no test signal or generator failed |
| qi | Qi wireless charging |  | none | no test signal or generator failed |
| fmmon | FM modulation monitor |  | none | no test signal or generator failed |
| ammon | AM modulation monitor |  | none | no test signal or generator failed |
| plnoise | Power-line noise locator |  | none | no test signal or generator failed |
| radar | Radar pulse analyser |  | none | no test signal or generator failed |
| gnssjam | GNSS interference detector |  | none | no test signal or generator failed |
| passband | Passband signal classifier |  | none | no test signal or generator failed |
| vlfmsk | VLF MSK transmitter monitor |  | none | no test signal or generator failed |
| occupancy | Spectrum occupancy monitor |  | none | no test signal or generator failed |
| evm | Modulation quality (MER) meter |  | none | no test signal or generator failed |
| freqacc | Carrier frequency accuracy meter |  | none | no test signal or generator failed |
| pttlog | PTT and duty-cycle logger |  | none | no test signal or generator failed |
| intermod | Intermodulation finder |  | none | no test signal or generator failed |
| spur | Spur and image finder |  | none | no test signal or generator failed |
| dopplerlog | Satellite Doppler curve logger |  | none | no test signal or generator failed |
| hop | Frequency-hopper analyser |  | none | no test signal or generator failed |
| birdie | Persistent carrier (birdie) finder |  | none | no test signal or generator failed |
| burst | Burst finder |  | none | no test signal or generator failed |
| fading | Fading analyser |  | none | no test signal or generator failed |
| inputhealth | SDR input health meter |  | none | no test signal or generator failed |
| phasenoise | Phase noise meter |  | none | no test signal or generator failed |
| ssbtest | SSB transmitter test meter |  | none | no test signal or generator failed |
| sinad | FM SINAD meter |  | none | no test signal or generator failed |
| fskan | FSK parameter analyser |  | none | no test signal or generator failed |
| yfactor | Noise figure (Y-factor) meter |  | none | no test signal or generator failed |
| cwkeying | CW keying quality meter |  | none | no test signal or generator failed |
| noisefloor | Noise floor logger |  | none | no test signal or generator failed |
| nfmdev | Narrowband FM deviation and CTCSS meter |  | none | no test signal or generator failed |
| afsk | AFSK (TNC) tone tuner |  | none | no test signal or generator failed |
| ookpulse | OOK / ASK pulse analyser |  | none | no test signal or generator failed |
| txstability | Transmitter frequency stability meter |  | none | no test signal or generator failed |
| occbw | Occupied bandwidth meter |  | none | no test signal or generator failed |
| papr | Crest factor (PAPR / CCDF) meter |  | none | no test signal or generator failed |
| speckurt | Spectral kurtosis RFI finder |  | none | no test signal or generator failed |
| glitch | SDR stream glitch detector |  | none | no test signal or generator failed |
| marker | ILS marker beacons (75 MHz) |  | none | no test signal or generator failed |
| fskmodem | FSK modems (Bell 103, V.21, V.23, Bell 202) |  | none | no test signal or generator failed |
| ssrint | SSR interrogations (1030 MHz) |  | none | no test signal or generator failed |
| dcr | DCR (Japan digital convenience radio) |  | none | no test signal or generator failed |
| xpa | XPA (10 / 20 Bd) |  | none | no test signal or generator failed |
| xpa2 | XPA2 |  | none | no test signal or generator failed |
| crowd36 | CROWD36 |  | none | no test signal or generator failed |
| f01 | FSK200/500 (F01) |  | none | no test signal or generator failed |
| f06 | FSK200/1000 (F06, F06a) |  | none | no test signal or generator failed |
| gwfsk | GW FSK (100 Bd) |  | none | no test signal or generator failed |
| gsc | Golay paging (GSC) |  | none | no test signal or generator failed |
| stl | STL link (studio-to-transmitter) |  | none | no test signal or generator failed |
| scada | SCADA radio telemetry (Modbus, DNP3) |  | none | no test signal or generator failed |
| fleetsync | Kenwood FleetSync |  | none | no valid output on the clean test signal (8 and 32 s) |
| autospec | AUTOSPEC |  | none | no valid output on the clean test signal (8 and 32 s) |
| spreadhf | SPREAD |  | none | no valid output on the clean test signal (8 and 32 s) |
| qrss | QRSS grabber |  | none | no valid output on the clean test signal (8 and 32 s) |
| dfcw | DFCW grabber |  | none | no valid output on the clean test signal (8 and 32 s) |
| riometer | Riometer |  | none | no valid output on the clean test signal (8 and 32 s) |
| pulsar | Pulsar folding |  | none | no valid output on the clean test signal (8 and 32 s) |
| xlinknb | Extra links, narrow |  | none | no test signal or generator failed |
| cdma2000 | CDMA2000 cells |  | none | no valid output on the clean test signal (8 and 32 s) |
| navic | NavIC |  | none | no valid output on the clean test signal (8 and 32 s) |
| ft8 | FT8 |  | none | no valid output on the clean test signal (8 and 32 s) |
| ft4 | FT4 |  | none | no valid output on the clean test signal (8 and 32 s) |
| wspr | WSPR |  | none | no valid output on the clean test signal (8 and 32 s) |
| reticulum | Reticulum |  | none | no test signal or generator failed |
| elrs | ExpressLRS |  | none | no test signal or generator failed |
| mavlink | MAVLink (SiK) |  | none | no test signal or generator failed |
| frsky | FrSky |  | none | no test signal or generator failed |
| flysky | FlySky |  | none | no test signal or generator failed |
| sfhss | Futaba S-FHSS |  | none | no test signal or generator failed |
| spektrum | Spektrum DSM |  | none | no test signal or generator failed |
| wfbng | wfb-ng / OpenHD |  | none | no test signal or generator failed |
| npr | New Packet Radio |  | none | no test signal or generator failed |
| winmor | WINMOR |  | none | no test signal or generator failed |
| mioty | mioty |  | none | no test signal or generator failed |
| dectnrp | DECT NR+ |  | none | no test signal or generator failed |
| navtex | NAVTEX |  | none | no valid output on the clean test signal (8 and 32 s) |
| jt65 | JT65 |  | none | no valid output on the clean test signal (8 and 32 s) |
| jt9 | JT9 |  | none | no valid output on the clean test signal (8 and 32 s) |
| q65 | Q65 |  | none | no valid output on the clean test signal (8 and 32 s) |
| fst4 | FST4 |  | none | no valid output on the clean test signal (8 and 32 s) |
| msk144 | MSK144 |  | none | no valid output on the clean test signal (8 and 32 s) |
| jt4 | JT4 |  | none | no valid output on the clean test signal (8 and 32 s) |
