# Mode numbers

Every mode has three numbers that must be unique:

- **family**: the mode's row in the mode list (`app/ui/mode_table.cpp`, `App::family`, `kNumFamilies` in `app/app.h`);
- **engine standard** = family + 2 (`Engine::setStandard`, `ModeTuning::stdMode`, `makeModeSynth`, the TX registry);
- **active standard** = engine standard - 1 = family + 1 (`Engine::activeStandard`, `RxTelemetry::standard`, reset / configure /
  poll / summary switches);

plus, for a receiver that reports telemetry, a **poll slot** (`seqs[N]` in `core/src/engine/engine_telemetry.cpp`, below
`Engine::kPollSeqSlots`). Plugin modes start at family 999 (engine standard 1001+); the Mode Builder row is 19999.

`tests/app/test_mode_numbers.cpp` (ctest `mode_numbers`) fails on a duplicate family, a duplicate engine standard in the registry,
a poll slot used by two receivers, or a number outside `kNumFamilies` / `kPollSeqSlots`. `test_mode_numbers --markdown` prints the
full table of the current tree.

Settings are kept per family under the family number (`app_state.cpp` slot keys) and per protocol entry under its name. None of
the modes renumbered for 3.0 was ever released, so no saved setting moves.

## Allocations of 3.0

Several branches picked numbers on their own; the later ones were moved (integ-30).

| mode | family | engine std | active std | poll slot | branch | note |
|---|---|---|---|---|---|---|
| HF waveform ID | 145 | 147 | 146 | 135 | (0.2.9) | |
| KLW (railway end of train) | 146 | 148 | 147 | 147 | (0.2.9, PR #231) | released, kept |
| TDCS (railway dispatch) | 147 | 149 | 148 | 148 | (0.2.9, PR #231) | released, kept |
| NFC / RFID 13.56 MHz | 148 | 150 | 149 | 141 | mode-nfc-030 | was family 146 |
| SBAS | 150 | 152 | 151 | 150 | mode-sbas-radar-030 | |
| ASTERIX | 151 | 153 | 152 | - | mode-sbas-radar-030 | no radio receiver |
| Argos | 152 | 154 | 153 | 143 | mode-argos-sigfox-030 | was family 148 |
| Sigfox | 153 | 155 | 154 | 144 | mode-argos-sigfox-030 | was family 149 |
| ISDB-S (BS) | 154 | 156 | 155 | 154 | mode-isdbs-030 | |
| Marine VHF voice | 155 | 157 | 156 | 142 | vhf-marine-030 | was family 148, slot 141 |
| Horus balloons | 156 | 158 | 157 | 157 | horus-030 | was family 148 |
| P25 Phase 2 | 180 | 182 | 181 | 181 | mode-p25-edacs-030 | |
| LoRaWAN (all channels) | 200 | 202 | 201 | 160 | mode-lora-030 | |
| LoRa satellites | 201 | 203 | 202 | 161 | mode-lora-030 | |
| DME | 121 | 123 | 122 | 162 | (0.2.9) | poll slot was 110, shared with DGPS: moved in 3.0 |
| FY-4 LRIT | 232 | 234 | 233 | 191 | feat36-fygeo | FengYun-4 LRIT, FY-4A (90 ksym/s) or FY-4B (120 ksym/s) as a choice; one receiver (`rxFy_`) for the three entries, told apart by the active standard |
| FY-4A HRIT-II/III | 233 | 235 | 234 | 192 | feat36-fygeo | |
| FY-2 S-VISSR | 234 | 236 | 235 | 193 | feat36-fygeo | |
| PSK demodulator (BPSK / QPSK analyser) | 202 | 204 | 203 | 163 | issue-modes-030 | #282 |
| PACTOR-I (monitor) | 203 | 205 | 204 | 164 | issue-modes-030 | #216 |
| Flipper Zero Sub-GHz chat | 204 | 206 | 205 | 165 | (3.1) | row added with the 3.2 modes below |
| ROS (HF detector) | 205 | 207 | 206 | 182 | feat32-modes2 | detection only, text needs a public spec |
| SSDV pictures | 206 | 208 | 207 | 183 | feat32-modes2 | fsphil/ssdv |
| EBICab / Eurobalise | 207 | 209 | 208 | 184 | feat32-modes2 | needs a user test |
| Inmarsat (all carriers) | 208 | 210 | 209 | 185 | feat33-inmarsat | 3.3; numbers may need to move at merge |
| Internet TV (IPTV) | 209 | 211 | 210 | - | feat34-iptv | no radio, no receiver, no poll slot; 3.4 |
| nRF24L01 | 210 | 212 | 211 | 186 | feat34-iqtcp-nrf24 | 3.4; needs a user test; numbers may need to move at merge |
| ATIS (inland waterways) | 211 | 213 | 212 | 187 | feat35-lane-b | 3.5; needs a user test; numbers may need to move at merge |
| Shortwave | 212 | 214 | 213 | 188 | feat35-shortwave | 3.5; the general receiver with one-click HF decoders; numbers may need to move at merge |
| GK-2A LRIT | 230 | 232 | 231 | 189 | feat36-gk2a | 3.6; BPSK 128 ksym/s; needs a user test (no live signal); numbers may need to move at merge |
| GK-2A HRIT | 231 | 233 | 232 | 190 | feat36-gk2a | 3.6; QPSK 3 Msym/s; needs a user test (no live signal); numbers may need to move at merge |
| FY-3 AHRPT | 235 | 237 | 236 | 195 | feat36-fy3 | 3.6; Fengyun-3 polar AHRPT (VIRR, MERSI pictures); no live signal tested; numbers may need to move at merge |
| FY-3 MPT | 236 | 238 | 237 | 196 | feat36-fy3 | 3.6; Fengyun-3 A / B / C medium rate picture transmission (MERSI); no live signal tested; numbers may need to move at merge |
| PACTOR-II | 237 | 239 | 238 | 197 | feat36-pactor | 3.6; physical layer only (carriers, clock, symbols, frame period); one real recording; numbers may need to move at merge |
| PACTOR-III | 238 | 240 | 239 | 198 | feat36-pactor | 3.6; physical layer only; synthetic signals only; numbers may need to move at merge |
| PACTOR-IV | 239 | 241 | 240 | 199 | feat36-pactor | 3.6; physical layer only (1800 baud single carrier and its constellation); synthetic signals only; numbers may need to move at merge |
| SITOR-B | 119 | 121 | 120 | 194 | feat36-modes | 3.6; FEC teletype text (NAVTEX and HF coast stations) through the marine FSK front end and the NAVTEX framing; synthetic NAVTEX signals only; the family and standards date from the WIP row, only the poll slot is new |
| SELCAL (aircraft tone call) | 240 | 242 | 241 | 200 | feat36-dsc | 3.6; own mode for the SELCAL detector of the tone decoders (call log, codes, aircraft table); synthetic signals only; numbers may need to move at merge |
| Sub-GHz sensors | 241 | 243 | 242 | 201 | feat37-sensors | 3.7; 433.92 MHz first (315 / 868 / 915 MHz too); decoders vendored from rtl_433, see docs/SENSORS-3.7.md; numbers may need to move at merge |
| Sferics (lightning) | 243 | 245 | 244 | 203 | feat37-modesB | 3.7; broadband impulse counter, mains-train rejection; synthetic signals only; family 242 / std 244 / 243 / slot 202 are left for feat37-modesA; numbers may need to move at merge |
| Thread | 244 | 246 | 245 | 204 | feat37-discord5 | 3.7; 802.15.4 receiver in Thread mode (beacons, 6LoWPAN / MLE where clear, encrypted traffic shown as encrypted); synthetic signals only; numbers may need to move at merge (feat37-modesB took 242 / 243) |
| Inmarsat BGAN | 245 | 247 | 246 | 205 | feat37-bgan | 3.7; forward bearers, broadcast signalling only (docs/BGAN-3.7.md); synthetic signals only; numbers may need to move at merge |
| NOAA APT | 260 | 262 | 261 | 220 | feat37-sd1 | 3.7; analogue pictures, retired 2025, recordings only; numbers may need to move at merge |
| NOAA HRPT | 261 | 263 | 262 | 221 | feat37-sd1 | 3.7; PM Manchester, retired 2025, recordings only; numbers may need to move at merge |
| NOAA GAC | 262 | 264 | 263 | 222 | feat37-sd1 | 3.7; retired 2025, recordings only; numbers may need to move at merge |
| NOAA DSB | 263 | 265 | 264 | 223 | feat37-sd1 | 3.7; TIP / HIRS, retired 2025, recordings only; numbers may need to move at merge |
| Meteor HRPT | 264 | 266 | 265 | 224 | feat37-sd1 | 3.7; MSU-MR at 1700 MHz; numbers may need to move at merge |
| Elektro-L RDAS | 265 | 267 | 266 | 225 | feat37-sd1 | 3.7; MSU-GS, X-band 30.72 Msym/s; numbers may need to move at merge |
| Arktika-M RDAS | 266 | 268 | 267 | 226 | feat37-sd1 | 3.7; MSU-GS, X-band 30.72 Msym/s; numbers may need to move at merge |
| Arktika-M GGAK | 267 | 269 | 268 | 227 | feat37-sd1 | 3.7; 1703 MHz PM; numbers may need to move at merge |
| AWS L-band | 268 | 270 | 269 | 228 | feat37-sd1 | 3.7; MWR, 1707 MHz; numbers may need to move at merge |
| MSG Raw Data | 269 | 271 | 270 | 229 | feat37-sd2 | 3.8; SEVIRI, 1686.83 MHz; numbers may need to move at merge |
| HimawariCast | 270 | 272 | 271 | 230 | feat37-sd2 | 3.8; Fazzt files on DVB-S2, C-band; numbers may need to move at merge |
| GeoNetCast | 271 | 273 | 272 | 231 | feat37-sd2 | 3.8; Fazzt files on DVB-S2, C-band; numbers may need to move at merge |
| ADS-L | 300 | 302 | 301 | 260 | feat38-adsl | 3.8; EASA ADS-L 4 SRD-860 on 868.2 / 868.4 / 869.525 MHz, feeds the FLARM receiver from the same capture (docs/ADSL-3.8.md); no live signal tested; numbers may need to move at merge |
| RC and mesh links (Reticulum, ExpressLRS, MAVLink SiK, FrSky, FlySky) | 360 | 362 | 361 | 360 | feat38-mesh-rc | 3.8; protocol entries through subModes (362 masks 1-16); simulated signals only; family block 360-379; numbers may need to move at merge |
| FANET | 400 | 402 | 401 | 300 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| OGN tracker | 401 | 403 | 402 | 301 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| GBAS VDB (bursts only) | 402 | 404 | 403 | 302 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| LDACS (structure) | 403 | 405 | 404 | 303 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| AeroMACS (structure) | 404 | 406 | 405 | 304 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| AAR end of train | 405 | 407 | 406 | 305 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| DSRC road toll | 406 | 408 | 407 | 306 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| 802.11p V2X | 407 | 409 | 408 | 307 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| C-V2X (structure) | 408 | 410 | 409 | 308 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| ERMES | 409 | 411 | 410 | 309 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| Marine radar | 410 | 412 | 411 | 310 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| Weather radar | 411 | 413 | 412 | 311 | feat38-avia-road | 3.8; one receiver (`rxAvRoad_`), see docs/AVIA-ROAD-3.8.md; numbers may need to move at merge |
| GOES GRB | 290 | 292 | 291 | 290 | feat38-sat-sd3 | 3.8; DVB-S2, ABI / SUVI / GLM; numbers may need to move at merge |
| GOES GVAR | 291 | 293 | 292 | 291 | feat38-sat-sd3 | 3.8; GOES-N, retired, recordings only; numbers may need to move at merge |
| GOES CDA | 292 | 294 | 293 | 292 | feat38-sat-sd3 | 3.8; GOES-R 40k / 4k, GOES-N PM; numbers may need to move at merge |
| GOES MDL | 293 | 295 | 294 | 293 | feat38-sat-sd3 | 3.8; GOES-N, frames only; numbers may need to move at merge |
| GK-2A UHRIT | 294 | 296 | 295 | 294 | feat38-sat-sd3 | 3.8; DVB-S2 at X-band; numbers may need to move at merge |
| GK-2A CDAS | 295 | 297 | 296 | 295 | feat38-sat-sd3 | 3.8; frames only; numbers may need to move at merge |
| S-band CCSDS | 296 | 298 | 297 | 296 | feat38-sat-sd3 | 3.8; satellite table, 46 links; numbers may need to move at merge |
| Geoscan | 297 | 299 | 298 | 297 | feat38-sat-sd3 | 3.8; six satellites, 66-byte frames; numbers may need to move at merge |
| Lucky-7 | 298 | 300 | 299 | 298 | feat38-sat-sd3 | 3.8; frames; numbers may need to move at merge |
| Spino | 299 | 301 | 300 | 299 | feat38-sat-sd3 | 3.8; frames; numbers may need to move at merge |
| Erminaz | 510 | 512 | 511 | 510 | feat38-sat-sd3 | 3.8; CCSDS frames; numbers may need to move at merge |
| QRSS grabber | 460 | 462 | 461 | 461 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| DFCW grabber | 461 | 463 | 462 | 462 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| FSK441 | 462 | 464 | 463 | 463 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| ISCAT | 463 | 465 | 464 | 464 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| PI4 | 464 | 466 | 465 | 465 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| Throb / ThrobX | 465 | 467 | 466 | 466 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| SuperSID (VLF) | 500 | 502 | 501 | 501 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| Riometer | 501 | 503 | 502 | 502 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| Pulsar folding | 502 | 504 | 503 | 503 | feat38-ham-science | 3.8; simulated signals only; numbers may need to move at merge |
| JT6M | 750 | 752 | 751 | 751 | feat39-misc | 3.9; simulated signals only |
| Schumann resonance monitor | 820 | 822 | 821 | 821 | feat39-w1b | 3.9; ham science event monitor (hamsci kind 10); simulated signals only |
| Whistler and tweek detector | 823 | 825 | 824 | 824 | feat39-w1b | 3.9; ham science event monitor (hamsci kind 11); simulated signals only |
| Moon echo (EME) test | 826 | 828 | 827 | 827 | feat39-w1b | 3.9; ham science event monitor (hamsci kind 12); simulated signals only |
| Radio Jove | 829 | 831 | 830 | 830 | feat39-w1b | 3.9; ham science event monitor (hamsci kind 13); simulated signals only |
| e-CALLISTO solar bursts | 832 | 834 | 833 | 833 | feat39-w1b | 3.9; ham science event monitor (hamsci kind 14); simulated signals only |
| Aurora / Es / TEP beacons | 835 | 837 | 836 | 836 | feat39-w1b | 3.9; ham science event monitor (hamsci kind 15); simulated signals only |
| UMTS cells | 480 | 482 | 481 | 440 | feat38-broadcast | 3.8; 3G cell search by primary scrambling code (no BCH); synthetic signals only; numbers may need to move at merge |
| JPSS HRD (NPP, NOAA-20) | 530 | 532 | 531 | 530 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| JPSS HRD (NOAA-21) | 531 | 533 | 532 | 531 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| EOS DB (Aqua, Aura) | 532 | 534 | 533 | 532 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| EOS DB (Terra) | 533 | 535 | 534 | 533 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| MetOp X-band dump | 534 | 536 | 535 | 534 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| MetOp-SG DDB | 535 | 537 | 536 | 535 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Landsat 8/9 X-band | 536 | 538 | 537 | 536 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Landsat 8/9 S-band | 537 | 539 | 538 | 537 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Proba S-band dump | 538 | 540 | 539 | 538 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Proba-V X-band dump | 539 | 541 | 540 | 539 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| GCOM-W1 | 540 | 542 | 541 | 540 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| GCOM-C1 | 541 | 543 | 542 | 541 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| STEREO high rate | 542 | 544 | 543 | 542 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Oceansat-2 DB | 543 | 545 | 544 | 543 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Oceansat-3 L-band | 544 | 546 | 545 | 544 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Saral L-band | 545 | 547 | 546 | 545 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Oceansat-3 X-band dump | 546 | 548 | 547 | 546 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Saral X-band dump | 547 | 549 | 548 | 547 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| Kanopus-V | 548 | 550 | 549 | 548 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| DMSP RTD | 549 | 551 | 550 | 549 | feat38-sat-xband | 3.8; numbers may need to move at merge |
| CDMA2000 cells | 660 | 662 | 661 | 660 | feat39-cell | 3.9; IS-95 / cdma2000 pilot + Sync Channel; synthetic signals only |
| TD-SCDMA cells | 661 | 663 | 662 | 661 | feat39-cell | 3.9; DwPTS SYNC-DL + TS0 midamble cell parameter, no BCH; synthetic signals only |
| Lightning position (network) | 790 | 792 | 791 | 790 | feat39-ltpos | 3.9; TDOA from several stations, see docs/LIGHTNING-POSITION-3.9.md; simulated signals only |
| HF packet 300 Bd | 800 | 802 | 801 | 801 | feat39-w1a | 3.9; AX.25 over 300 Bd AFSK, full decode; simulated signals only |
| Voice inversion scrambler | 801 | 803 | 802 | 802 | feat39-w1a | 3.9; full-band inversion detection and descrambling; simulated signals only |
| ELT 121.5 MHz | 802 | 804 | 803 | 803 | feat39-w1a | 3.9; swept-tone detection only; simulated signals only |
| Fleet Broadcast (UHF SATCOM) | 870 | 872 | 871 | 870 | feat39-fbr | 3.9; 16 x 75 Bd UHF SATCOM multiplex, framing and channel activity only, see docs/FBR-3.9.md; simulated signals only |
| CP16 | 880 | 882 | 881 | 880 | feat39-cp16 | 3.9; CRAC 16-carrier picture text, receive and transmit, see docs/CP16-3.9.md (renumbered from 790-792 at the merge: those are Lightning position); simulated only (plus two CP16+ software files, not off-air) |
| RC model PPM (72 MHz) | 881 | 883 | 882 | 881 | feat40-modes1 | 4.0; PPM channel count and pulse spacings, see docs/X40-4.0.md; simulated signals only |
| UVB-76 buzzer monitor | 882 | 884 | 883 | 882 | feat40-modes1 | 4.0; buzz train timing only, voice not decoded; simulated signals only |
| GRAVES meteor echoes | 883 | 885 | 884 | 883 | feat40-modes1 | 4.0; carrier and meteor echo finder; simulated signals only |
| Radio Jove bursts | 884 | 886 | 885 | 884 | feat40-modes1 | 4.0; power burst finder; simulated signals only |
| Beacon propagation monitor | 885 | 887 | 886 | 885 | feat40-modes1 | 4.0; steady / fading / aurora-like path class of one beacon; simulated signals only |
| EM4100 LF RFID (125 kHz) | 886 | 888 | 887 | 886 | feat40-modes2 | 4.0; simulated signals only (docs/EM4100-4.0.md) |
| UHF RFID reader (EPC Gen2) | 887 | 889 | 888 | 887 | feat40-modes2 | 4.0; simulated signals only (docs/UHFRFID-4.0.md) |
| Qi wireless charging | 888 | 890 | 889 | 888 | feat40-modes2 | 4.0; simulated signals only (docs/QI-4.0.md) |
| FM modulation monitor | 889 | 891 | 890 | 889 | feat40-modes2 | 4.0; simulated signals only (docs/FMMON-4.0.md) |
| AM modulation monitor | 890 | 892 | 891 | 890 | feat40-modes2 | 4.0; simulated signals only (docs/AMMON-4.0.md) |
| Power-line noise locator | 891 | 893 | 892 | 891 | feat40-modes2 | 4.0; simulated signals only (docs/PLNOISE-4.0.md) |
| Radar pulse analyser | 892 | 894 | 893 | 892 | feat40-modes2 | 4.0; simulated signals only (docs/RADAR-4.0.md) |
| GNSS interference detector | 893 | 895 | 894 | 893 | feat40-modes2 | 4.0; simulated signals only (docs/GNSSJAM-4.0.md) |
| Passband signal classifier | 894 | 896 | 895 | 894 | feat40-modes2 | 4.0; simulated signals only (docs/PASSBAND-4.0.md) |
| VLF MSK transmitter monitor | 895 | 897 | 896 | 895 | feat40-modes2 | 4.0; simulated signals only (docs/VLFMSK-4.0.md) |
| Plugins | 999 | 1001 | - | - | | |
| Mode Builder | 19999 | - | - | - | mode-builder-030 | |
| custom modes (Mode Builder) | 20000+ | family + 2 | - | - | mode-builder-030 | kCustomFamilyBase, saved by number in modes/registered.txt |

Protocol entries (a row with a subId, such as FT8 in HF digital or POCSAG in pagers) share their family on purpose; the test checks
that each (family, subId) pair is unique instead.

`kNumFamilies` = 601, `kPollSeqSlots` = 602 (families 213 to 234 and poll slots 189 to 194 are kept for the other 3.6 lanes).

A new mode takes the next free family above the highest one in this table and the next free poll slot, and adds its row here.
