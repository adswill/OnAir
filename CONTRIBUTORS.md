# Contributors

OnAir exists because of the people below: they sent fixes, reported bugs, shared recordings, translated and suggested ideas. The maintainer, [Kristijonas Adomaitis](https://github.com/adswill), is the author and is not repeated here. Only names people already use in public are shown. **Want your name changed or removed? Tell us on Discord or open an issue.**

In the program: Help, Credits, tab Contributors. To add someone, edit `tools/contributors.tsv` and run `python3 tools/gen_credits.py`.

## People (89)

| Who | What | Kind | Where | Reference |
|---|---|---|---|---|
| [0xVANISHED](https://github.com/0xVANISHED) | Faster ATSC receiver on Intel/Linux, Airspy notes | optimisation | ATSC | [#1](https://github.com/adswill/OnAir/issues/1) |
| [A2ureIm4la](https://github.com/A2ureIm4la) | DVB-T reception with a HackRF: DC spike, FEC sync backlog, filter width | fix | DVB | [#11](https://github.com/adswill/OnAir/issues/11) |
| [A2ureIm4la](https://github.com/A2ureIm4la) | Maps zoom to 19, working buttons, tiles menu; stale tiles dropped | pull request | ADS-B; ACARS | [#21](https://github.com/adswill/OnAir/issues/21), [#22](https://github.com/adswill/OnAir/issues/22) |
| [A2ureIm4la](https://github.com/A2ureIm4la) | Channel width shown on every spectrum | pull request | Interface | [#27](https://github.com/adswill/OnAir/issues/27) |
| [A2ureIm4la](https://github.com/A2ureIm4la) | Saved Stations library | pull request | FM | [#123](https://github.com/adswill/OnAir/issues/123) |
| [andriux26](https://github.com/andriux26) | Live mode switch | fix | Interface | [#191](https://github.com/adswill/OnAir/issues/191) |
| [andriux26](https://github.com/andriux26) | SpyServer network radio | pull request | Network radios | [#107](https://github.com/adswill/OnAir/issues/107) |
| [andriux26](https://github.com/andriux26) | Editable TLE source list, Keplerpass download | pull request | Satellite passes | [fb4a52aa](https://github.com/adswill/OnAir/commit/fb4a52aa), [4b277a93](https://github.com/adswill/OnAir/commit/4b277a93) |
| [andriux26](https://github.com/andriux26) | Lithuanian network radio dialog | translation | Languages | [992e89fc](https://github.com/adswill/OnAir/commit/992e89fc) |
| [ar8001](https://github.com/ar8001) | DVB-T/T2 not locked on Airspy Mini | bug report | DVB | [#163](https://github.com/adswill/OnAir/issues/163) |
| [Arclamp (CasualArclamp)](https://github.com/CasualArclamp) | DecSAT, a satellite receiver: its published description of PCM/PSK/PM satellite beacons (bi-phase data on a subcarrier, IRIG 106 and CCSDS sync words, the minor frame counter) identified the format of the recorded beacon, and its table of published Ku-band beacons is used to estimate the satellite; reference for the DVB-S work | idea | TV sat beacon decoder | [cc7a56e1](https://github.com/adswill/OnAir/commit/cc7a56e1), [e235347b](https://github.com/adswill/OnAir/commit/e235347b) |
| [Arclamp (CasualArclamp)](https://github.com/CasualArclamp) | DecSAT's GSE and IP crates and its MPE extractor (GSE variants and their automatic choice, vendor CRC trailer, IP validation, fragments, flow statistics, PCAP, MPE) ported to C++ for IP data over DVB-S/S2; allowed by the author | pull request | DVB-S/S2 | [a38b6ecd](https://github.com/adswill/OnAir/commit/a38b6ecd) |
| [Axpelle](https://github.com/Axpelle) | NAVTEX, airband range and weak WEFAX reports | bug report | NAVTEX; Airband; Weather fax | [#25](https://github.com/adswill/OnAir/issues/25), [#42](https://github.com/adswill/OnAir/issues/42), [#52](https://github.com/adswill/OnAir/issues/52), [#103](https://github.com/adswill/OnAir/issues/103) |
| [bellx2](https://github.com/bellx2) | RTL-SDR on macOS: reading and converting on separate threads | pull request | Radios | [#447](https://github.com/adswill/OnAir/issues/447) |
| [BI8SYN](https://github.com/BI8SYN) | Chinese UI fonts cut to a small size and made consistent on all desktop platforms | pull request | Languages | [#437](https://github.com/adswill/OnAir/issues/437) |
| [bithunter-99](https://github.com/bithunter-99) | DECT receiver | idea | DECT cordless phones | [#94](https://github.com/adswill/OnAir/issues/94) |
| [BSDnux](https://github.com/BSDnux) | List of analogue and MAC TV systems | idea | MAC; Analog TV | [#15](https://github.com/adswill/OnAir/issues/15), [#73](https://github.com/adswill/OnAir/issues/73) |
| [carlosjs](https://github.com/carlosjs) | Basic LoRa decoder; Sigfox | idea | LoRa (raw) | [#20](https://github.com/adswill/OnAir/issues/20), [#170](https://github.com/adswill/OnAir/issues/170) |
| [catheno](https://github.com/catheno) | Airband channel not saved | bug report | Airband | [#148](https://github.com/adswill/OnAir/issues/148) |
| Chercheur | SoapyRemote on Windows | bug report | Network radios | Discord |
| [Debegege](https://github.com/Debegege) | DVB-T with a Pluto on Windows 11, 0.2.3 crash, spectrum height question | bug report | DVB | [#18](https://github.com/adswill/OnAir/issues/18), [#59](https://github.com/adswill/OnAir/issues/59), [#91](https://github.com/adswill/OnAir/issues/91) |
| [DG9BFC](https://github.com/DG9BFC) | Pop-out player and DVB-S range requests | idea | DVB-S/S2 | [#20](https://github.com/adswill/OnAir/issues/20), [#30](https://github.com/adswill/OnAir/issues/30), [#93](https://github.com/adswill/OnAir/issues/93) |
| [DG9BFC](https://github.com/DG9BFC) | Pluto over network and USB tests, network radio and buffer requests | testing | Network radios | [#18](https://github.com/adswill/OnAir/issues/18), [#28](https://github.com/adswill/OnAir/issues/28), [#96](https://github.com/adswill/OnAir/issues/96), [#97](https://github.com/adswill/OnAir/issues/97) |
| [dhihacker](https://github.com/dhihacker) | Maldives ISDB-T and DVB-T/T2 8 MHz Pluto reports | bug report | ISDB-T; DVB | [#50](https://github.com/adswill/OnAir/issues/50), [#54](https://github.com/adswill/OnAir/issues/54), [#96](https://github.com/adswill/OnAir/issues/96) |
| [DickvanNoort](https://github.com/DickvanNoort) | Asked for credit to the code's developers | idea | Credits | [#190](https://github.com/adswill/OnAir/issues/190) |
| [dj1fti](https://github.com/dj1fti) | DAB+ scan CSV and Astrometa report | testing | DAB / DAB+ | [#179](https://github.com/adswill/OnAir/issues/179), [#180](https://github.com/adswill/OnAir/issues/180) |
| [DL5BCA](https://github.com/DL5BCA) | DVB-S below 700 MHz | idea | DVB-S/S2 | [#30](https://github.com/adswill/OnAir/issues/30) |
| [DL9DAK](https://github.com/DL9DAK) | ADS-B local antenna position and full map | idea | ADS-B | [#56](https://github.com/adswill/OnAir/issues/56), [#57](https://github.com/adswill/OnAir/issues/57) |
| [DME-3](https://github.com/DME-3) | Mode Builder: binary FSK acquisition stays centred for near-zero lead-ins | pull request | Mode Builder | [#461](https://github.com/adswill/OnAir/issues/461), [#460](https://github.com/adswill/OnAir/issues/460) |
| [dominik0815](https://github.com/dominik0815) | Pop-out fullscreen closing | bug report | DVB-S/S2 | [#71](https://github.com/adswill/OnAir/issues/71) |
| [drelectro](https://github.com/drelectro) | DVB-T timing steps carried into the pilot phase ramp; timing loop and TPS error correction | fix | DVB | [#8](https://github.com/adswill/OnAir/issues/8), [9298e71c](https://github.com/adswill/OnAir/commit/9298e71c), [0a9a6355](https://github.com/adswill/OnAir/commit/0a9a6355) |
| [drelectro](https://github.com/drelectro) | Smooth waterfall scrolling, single gain slider placement | fix | Interface | [#7](https://github.com/adswill/OnAir/issues/7), [#10](https://github.com/adswill/OnAir/issues/10) |
| [drelectro](https://github.com/drelectro) | P25 frame sync pattern | fix | P25 | [#77](https://github.com/adswill/OnAir/issues/77) |
| [drelectro](https://github.com/drelectro) | HFDL decodes on air (code polynomial order, preamble search) | fix | HFDL | [#175](https://github.com/adswill/OnAir/issues/175) |
| [drelectro](https://github.com/drelectro) | Tests build with MinGW on Windows | fix | Build | [#173](https://github.com/adswill/OnAir/issues/173) |
| [drelectro](https://github.com/drelectro) | DVB-T SSE2 Viterbi decoder and a cheaper acquisition search | optimisation | DVB | [79718e62](https://github.com/adswill/OnAir/commit/79718e62), [f0ad14e9](https://github.com/adswill/OnAir/commit/f0ad14e9) |
| [drelectro](https://github.com/drelectro) | HF digital runs from 250 ksps | optimisation | FT8 | [#76](https://github.com/adswill/OnAir/issues/76) |
| [drelectro](https://github.com/drelectro) | Hermes Lite 2 network radio | pull request | Network radios | [#75](https://github.com/adswill/OnAir/issues/75) |
| [drelectro](https://github.com/drelectro) | RFNM native receive driver | pull request | Radios | [#149](https://github.com/adswill/OnAir/issues/149) |
| [drelectro](https://github.com/drelectro) | Wi-SUN FAN decoder with regional band plans | pull request | Wi-SUN FAN | [#147](https://github.com/adswill/OnAir/issues/147) |
| [drelectro](https://github.com/drelectro) | ANZ 915 MeshCore Mid band plan | pull request | MeshCore | [#124](https://github.com/adswill/OnAir/issues/124) |
| [drelectro](https://github.com/drelectro) | P25 follower keeps decoding the control channel in follow-only mode; first call heard | pull request | P25 | [#347](https://github.com/adswill/OnAir/issues/347) |
| [drfruct](https://github.com/drfruct) | DMR Whole span: the sound goes to a voice call, not to a carrier only locked | pull request | DMR | [#352](https://github.com/adswill/OnAir/issues/352) |
| [ea4gja](https://github.com/ea4gja) | Amateur television and PAL-FM requests | idea | Analog TV | [#13](https://github.com/adswill/OnAir/issues/13), [#15](https://github.com/adswill/OnAir/issues/15) |
| [ea4hwg](https://github.com/ea4hwg) | SDRplay RSPduo tuner 2 API error | bug report | Radios | [#31](https://github.com/adswill/OnAir/issues/31) |
| [f0cyf](https://github.com/f0cyf) | HFDL, Orbcomm, VDL4 and AIS reports | bug report | HFDL; Orbcomm; VDL4; AIS | [#66](https://github.com/adswill/OnAir/issues/66), [#181](https://github.com/adswill/OnAir/issues/181), [#182](https://github.com/adswill/OnAir/issues/182), [#87](https://github.com/adswill/OnAir/issues/87) |
| [f5gkw](https://github.com/f5gkw) | Mesh, fldigi modes and TETRAPOL requests | idea | TETRAPOL; Meshtastic | [#26](https://github.com/adswill/OnAir/issues/26), [#105](https://github.com/adswill/OnAir/issues/105), [#106](https://github.com/adswill/OnAir/issues/106), [#108](https://github.com/adswill/OnAir/issues/108), [#113](https://github.com/adswill/OnAir/issues/113) |
| [F5OEO](https://github.com/F5OEO) | PlutoSDR at native 64/7 Msps for DVB-T/T2; AVX2 resampler | optimisation | DVB | [#33](https://github.com/adswill/OnAir/issues/33), [#34](https://github.com/adswill/OnAir/issues/34) |
| [F5OEO](https://github.com/F5OEO) | DVB-S2 8-bit LDPC decoder for high code rates | optimisation | DVB-S/S2 | [#83](https://github.com/adswill/OnAir/issues/83) |
| [F5OEO](https://github.com/F5OEO) | DVB-C real time on a 10 Msps radio, stream clock fix | optimisation | DVB-C | [#82](https://github.com/adswill/OnAir/issues/82) |
| [F5OEO](https://github.com/F5OEO) | Network PlutoSDR boards, Tezuka firmware, CS8/CS16 | pull request | Network radios | [#32](https://github.com/adswill/OnAir/issues/32) |
| [F5OEO](https://github.com/F5OEO) | FM video for analog FPV links and analogue TV up to 6 GHz | pull request | Analog TV | [#36](https://github.com/adswill/OnAir/issues/36) |
| [F5OEO](https://github.com/F5OEO) | ATSC 3.0 real-signal decoding, ALP/TS delivery, faster LDPC and bootstrap search | pull request | ATSC 3.0 | [#81](https://github.com/adswill/OnAir/issues/81), [#98](https://github.com/adswill/OnAir/issues/98), [#100](https://github.com/adswill/OnAir/issues/100) |
| [F5OEO](https://github.com/F5OEO) | DAB PAD, FIC analysis, Analyzer tab, announcements, recording; cheaper on ARM boards | pull request | DAB / DAB+ | [#47](https://github.com/adswill/OnAir/issues/47), [bb92f799](https://github.com/adswill/OnAir/commit/bb92f799) |
| [F5OEO](https://github.com/F5OEO) | ISDB-T decodes the real signal, FEC on a worker thread | pull request | ISDB-T | [#86](https://github.com/adswill/OnAir/issues/86) |
| [F5OEO](https://github.com/F5OEO) | GSM-R broadcast information | pull request | GSM-R | [#41](https://github.com/adswill/OnAir/issues/41) |
| [F5OEO](https://github.com/F5OEO) | Pluto: the native driver scans the network and asks boards for their firmware | pull request | Network radios | [#345](https://github.com/adswill/OnAir/issues/345) |
| [fe8769a](https://github.com/fe8769a) | Build crash on DragonOS | bug report | Build | [#43](https://github.com/adswill/OnAir/issues/43) |
| [ffishman](https://github.com/ffishman) | Larger sample buffers for network Plutos; rtl_tcp greeting wait; headless --net-radio | fix | Network radios | [#119](https://github.com/adswill/OnAir/issues/119), [#186](https://github.com/adswill/OnAir/issues/186), [#187](https://github.com/adswill/OnAir/issues/187) |
| [ffishman](https://github.com/ffishman) | Windows: files in folders with accents | fix | Windows | [#168](https://github.com/adswill/OnAir/issues/168) |
| [ffishman](https://github.com/ffishman) | Linux: bundled codec2 no longer crashes beside the system one | fix | FreeDV | [#174](https://github.com/adswill/OnAir/issues/174) |
| [ffishman](https://github.com/ffishman) | HFDL bursts decode again | fix | HFDL | [#177](https://github.com/adswill/OnAir/issues/177) |
| [ffishman](https://github.com/ffishman) | LongModerate frequency slot; ANZ 915 band plan; packets CSV columns; export and map at the receiver | pull request | Meshtastic; MeshCore | [#109](https://github.com/adswill/OnAir/issues/109), [#110](https://github.com/adswill/OnAir/issues/110), [#115](https://github.com/adswill/OnAir/issues/115), [#116](https://github.com/adswill/OnAir/issues/116), [#188](https://github.com/adswill/OnAir/issues/188) |
| [ffishman](https://github.com/ffishman) | Optional full RDS dwell in the FM scan | pull request | FM | [#111](https://github.com/adswill/OnAir/issues/111) |
| [ffishman](https://github.com/ffishman) | Brazilian Portuguese review and restore | translation | Languages | [#141](https://github.com/adswill/OnAir/issues/141), [#144](https://github.com/adswill/OnAir/issues/144) |
| [FleXoft](https://github.com/FleXoft) | macOS Sequoia run problem | bug report | macOS | [#3](https://github.com/adswill/OnAir/issues/3) |
| [frank-1225](https://github.com/frank-1225) | HFDL: ICAO octet bit order matches dumphfdl | pull request | HFDL | [#327](https://github.com/adswill/OnAir/issues/327) |
| [gmg2719](https://github.com/gmg2719) | 5G NR and LTE band scan requests; LimeSDR and UHD requests | idea | 5G NR cells; Cell towers | [#64](https://github.com/adswill/OnAir/issues/64), [#65](https://github.com/adswill/OnAir/issues/65), [#89](https://github.com/adswill/OnAir/issues/89) |
| [gmg2719](https://github.com/gmg2719) | Four DTMB IQ recordings and many decode reports | samples | DTMB | [#38](https://github.com/adswill/OnAir/issues/38), [#61](https://github.com/adswill/OnAir/issues/61), [#62](https://github.com/adswill/OnAir/issues/62), [#63](https://github.com/adswill/OnAir/issues/63), [#88](https://github.com/adswill/OnAir/issues/88), [#184](https://github.com/adswill/OnAir/issues/184) |
| [grahampaull](https://github.com/grahampaull) | Lost spectrum display | bug report | Interface | [#142](https://github.com/adswill/OnAir/issues/142) |
| [grue74](https://github.com/grue74) | DVB-C subtitles, teletext scaling and guide issues | bug report | DVB-C | [#112](https://github.com/adswill/OnAir/issues/112), [#125](https://github.com/adswill/OnAir/issues/125), [#126](https://github.com/adswill/OnAir/issues/126) |
| [gvanem](https://github.com/gvanem) | Windows crash findings and clang-cl notes (strftime) | fix | Windows | [#74](https://github.com/adswill/OnAir/issues/74), [#104](https://github.com/adswill/OnAir/issues/104), [#157](https://github.com/adswill/OnAir/issues/157) |
| [hgxqk](https://github.com/hgxqk) | CDR IQ recordings and DRA+ proposals | samples | CDR | [#121](https://github.com/adswill/OnAir/issues/121), [#138](https://github.com/adswill/OnAir/issues/138) |
| [hgxqk](https://github.com/hgxqk) | Android SDR notes | testing | Android | [#122](https://github.com/adswill/OnAir/issues/122) |
| [ic706mk2](https://github.com/ic706mk2) | Hermes Lite and QO-100 testing with a Pluto | testing | QO-100 | [#59](https://github.com/adswill/OnAir/issues/59), [#72](https://github.com/adswill/OnAir/issues/72) |
| [iu0azb](https://github.com/iu0azb) | IC-R8600 support | idea | Radios | [#136](https://github.com/adswill/OnAir/issues/136) |
| [iu1mrd](https://github.com/iu1mrd) | Airspy HF+ crash | bug report | Radios | [#160](https://github.com/adswill/OnAir/issues/160) |
| [IZ5RZR](https://github.com/IZ5RZR) | VDL2 and ACARS on a map | idea | VDL2; ACARS | [#99](https://github.com/adswill/OnAir/issues/99) |
| Jamiar | Squelch recording bug, with a test clip | bug report | Interface | Discord |
| Ken W4KRR | Memory panel | idea | Memory | Discord |
| [kevgfire](https://github.com/kevgfire) | UK band plan and USB/LSB requests | idea | General receiver | [#159](https://github.com/adswill/OnAir/issues/159), [#161](https://github.com/adswill/OnAir/issues/161) |
| [KvaKKK](https://github.com/KvaKKK) | Meshcore and Meshtastic preset editing | bug report | MeshCore; Meshtastic | [#60](https://github.com/adswill/OnAir/issues/60) |
| [lakisfm1](https://github.com/lakisfm1) | DAB station logo and FM scanning reports | bug report | DAB / DAB+; FM | [#45](https://github.com/adswill/OnAir/issues/45), [#46](https://github.com/adswill/OnAir/issues/46) |
| [mechanicalpotato](https://github.com/mechanicalpotato) | RTL-SDR V4 HF tuning | bug report | Radios | [#133](https://github.com/adswill/OnAir/issues/133) |
| [michaelgeorge001](https://github.com/michaelgeorge001) | Audio output for all modes; CHU time signal removal | idea | Time signals | [#140](https://github.com/adswill/OnAir/issues/140), [#150](https://github.com/adswill/OnAir/issues/150) |
| Mikki / YRPÄ | LTE band 31 | bug report | Cell towers | Discord |
| [modemuser](https://github.com/modemuser) | macOS packages target the macOS version they advertise | fix | macOS | [#4](https://github.com/adswill/OnAir/issues/4) |
| [mrwish7](https://github.com/mrwish7) | Airspy mini 10 MHz spectrum | idea | Radios | [#49](https://github.com/adswill/OnAir/issues/49) |
| [MUSTARDTIGERFPV](https://github.com/MUSTARDTIGERFPV) | SDR++ server support; Pluto added by IP no longer crashes | pull request | Network radios | [#171](https://github.com/adswill/OnAir/issues/171) |
| nature.van | Frequency database, audio recorder, fast scanner with noise floor, photo links, auto stop/start, support button | idea | Interface | Discord |
| [niuniu1138](https://github.com/niuniu1138) | Ionospheric sounder request | idea | Ionospheric sounders | [#80](https://github.com/adswill/OnAir/issues/80) |
| [nmaster2042](https://github.com/nmaster2042) | SpyServer support | idea | Network radios | [#68](https://github.com/adswill/OnAir/issues/68) |
| OM5NA | Fobos SDR native driver | pull request | Radios | [240c3c75](https://github.com/adswill/OnAir/commit/240c3c75) |
| [OnesAndZer0s](https://github.com/OnesAndZer0s) | FlexRadio support | idea | Radios | [#95](https://github.com/adswill/OnAir/issues/95) |
| [Pacjunk](https://github.com/Pacjunk) | DAB+ confused with multiple multiplexes | bug report | DAB / DAB+ | [#179](https://github.com/adswill/OnAir/issues/179) |
| [pbranly](https://github.com/pbranly) | macOS Sequoia DAB/DVB testing | testing | DAB / DAB+ | [#2](https://github.com/adswill/OnAir/issues/2), [#3](https://github.com/adswill/OnAir/issues/3), [#5](https://github.com/adswill/OnAir/issues/5) |
| [peterdewitte](https://github.com/peterdewitte) | Digital video generator tool | pull request | Tools | [#343](https://github.com/adswill/OnAir/issues/343) |
| [peterdewitte](https://github.com/peterdewitte) | Windows cameras opened by name and listed through DirectShow | pull request | Video input | [#346](https://github.com/adswill/OnAir/issues/346) |
| [peterdewitte](https://github.com/peterdewitte) | CI: Node 24 actions, pinned docker and setup-gradle actions, LF line endings | pull request | Interface | [#353](https://github.com/adswill/OnAir/issues/353), [#354](https://github.com/adswill/OnAir/issues/354), [#355](https://github.com/adswill/OnAir/issues/355) |
| [peterdewitte](https://github.com/peterdewitte) | Video generator: easy mode and a Preview tab (3.7) | pull request | Tools | [#444](https://github.com/adswill/OnAir/issues/444) |
| [peterdewitte](https://github.com/peterdewitte) | Video generator: hardware encoders that are listed also start | pull request | Tools | [#446](https://github.com/adswill/OnAir/issues/446) |
| [peterdewitte](https://github.com/peterdewitte) | Windows camera input kept in the release build, microphone remembered by name | pull request | Video input | [#445](https://github.com/adswill/OnAir/issues/445) |
| [peterdewitte](https://github.com/peterdewitte) | LoRa APRS transmit with its own packet and LoRa settings | pull request | LoRa APRS | [#467](https://github.com/adswill/OnAir/issues/467) |
| [PU2SWX](https://github.com/PU2SWX) | Portuguese (Brazil) interface translation | translation | Languages | [#117](https://github.com/adswill/OnAir/issues/117), [#141](https://github.com/adswill/OnAir/issues/141) |
| [py2gn](https://github.com/py2gn) | Crash on CPUs without AVX | bug report | Windows | [#74](https://github.com/adswill/OnAir/issues/74) |
| [PY2PLL](https://github.com/PY2PLL) | AirSpy Discovery stop and Meteor M2-3 LRPT reports | bug report | Meteor-M LRPT | [#78](https://github.com/adswill/OnAir/issues/78), [#79](https://github.com/adswill/OnAir/issues/79) |
| [ranaud80](https://github.com/ranaud80) | Diagnosis of DAB+ confusion between multiplexes | testing | DAB / DAB+ | [#179](https://github.com/adswill/OnAir/issues/179) |
| [RedmiNote5User](https://github.com/RedmiNote5User) | FM scanner coverage and CDR playback reports | bug report | FM; CDR | [#135](https://github.com/adswill/OnAir/issues/135), [#138](https://github.com/adswill/OnAir/issues/138) |
| [richt314](https://github.com/richt314) | Touch-friendly interface | idea | Interface | [#139](https://github.com/adswill/OnAir/issues/139) |
| [rlabs-oss](https://github.com/rlabs-oss) | libiio IP support | idea | Network radios | [#19](https://github.com/adswill/OnAir/issues/19) |
| [ryanginn](https://github.com/ryanginn) | DAB+ IQ file decoding | bug report | DAB / DAB+ | [#44](https://github.com/adswill/OnAir/issues/44) |
| [sbuchweitz](https://github.com/sbuchweitz) | Docker image showing the GUI in a browser | pull request | Docker | [#146](https://github.com/adswill/OnAir/issues/146) |
| Shadowman | Found the squawk JSON bug | bug report | ADS-B | Discord |
| [stdevPavelmc](https://github.com/stdevPavelmc) | DTMB 6 MHz request; SDRplay detection on Linux | idea | DTMB | [#16](https://github.com/adswill/OnAir/issues/16), [#17](https://github.com/adswill/OnAir/issues/17) |
| sugusllly | Ham It Down converter support | idea | Radios | Discord |
| [Sunnnset5](https://github.com/Sunnnset5) | Audio source and UDP export; services crash | idea | Interface | [#102](https://github.com/adswill/OnAir/issues/102), [#166](https://github.com/adswill/OnAir/issues/166) |
| [Tech500](https://github.com/Tech500) | Runtime assert error | bug report | Interface | [#53](https://github.com/adswill/OnAir/issues/53) |
| Topol (Pawel Topolski), in collaboration with AI | Icom IC-R8600 I/Q: a working macOS SoapySDR module and FX2 loader (MIT), the base of the native OnAir driver | pull request | Radios | Discord |
| [tortyl](https://github.com/tortyl) | E-czas radio time protocol request | idea | Time signals | [#155](https://github.com/adswill/OnAir/issues/155) |
| [tortyl](https://github.com/tortyl) | SoapySDR over LAN probe output and testing | testing | Network radios | [#84](https://github.com/adswill/OnAir/issues/84) |
| [trebor-56](https://github.com/trebor-56) | LimeSDR Mini request | idea | Radios | [#20](https://github.com/adswill/OnAir/issues/20), [#169](https://github.com/adswill/OnAir/issues/169) |
| [tsalisbury0](https://github.com/tsalisbury0) | HD Radio, LTE and NR scanning, HaLow requests | idea | HD Radio; Wi-Fi HaLow (802.11ah) | [#143](https://github.com/adswill/OnAir/issues/143), [#164](https://github.com/adswill/OnAir/issues/164), [#165](https://github.com/adswill/OnAir/issues/165), [#167](https://github.com/adswill/OnAir/issues/167) |
| [Ugo300](https://github.com/Ugo300) | Graves view | idea | Interface | [#92](https://github.com/adswill/OnAir/issues/92) |
| [unilabs-exp](https://github.com/unilabs-exp) | CSV export resolution and metric units | idea | Interface | [#85](https://github.com/adswill/OnAir/issues/85) |
| [Voyager-III](https://github.com/Voyager-III) | Fullscreen option | idea | Interface | [#118](https://github.com/adswill/OnAir/issues/118) |
| [WasyaTheWolf](https://github.com/WasyaTheWolf) | Globalstar labels, GNSS sync drops, Iridium features | bug report | Globalstar; GNSS; Iridium | [#189](https://github.com/adswill/OnAir/issues/189) |
| [WispAyr](https://github.com/WispAyr) | LoRa sync symbols as signed 7-bit values | fix | Meshtastic; MeshCore | [#24](https://github.com/adswill/OnAir/issues/24) |
| [WolfgangSchreiner](https://github.com/WolfgangSchreiner) | QO-100 and IP radios | idea | QO-100 | [#97](https://github.com/adswill/OnAir/issues/97) |
| [xpwina](https://github.com/xpwina) | DTMB decode report | bug report | DTMB | [#58](https://github.com/adswill/OnAir/issues/58) |
| [xxj737](https://github.com/xxj737) | MHEG-5 for DTMB, update mirror, Android build | idea | DTMB | [#176](https://github.com/adswill/OnAir/issues/176), [#183](https://github.com/adswill/OnAir/issues/183), [#122](https://github.com/adswill/OnAir/issues/122) |
| [xxj737](https://github.com/xxj737) | Tested AVS+ and DRA decoding, CDR tuning with Airspy HF+, DTMB labels | testing | DTMB; CDR | [#55](https://github.com/adswill/OnAir/issues/55), [#120](https://github.com/adswill/OnAir/issues/120), [#178](https://github.com/adswill/OnAir/issues/178) |
| [YZYMC](https://github.com/YZYMC) | Compile fixes in dvbstool | fix | Build | [#151](https://github.com/adswill/OnAir/issues/151), [#156](https://github.com/adswill/OnAir/issues/156) |
| [YZYMC](https://github.com/YZYMC) | AVS+/DRA decode and CUDA DTMB decoder requests | idea | DTMB; CDR | [#39](https://github.com/adswill/OnAir/issues/39), [#55](https://github.com/adswill/OnAir/issues/55) |
| [YZYMC](https://github.com/YZYMC) | LBJ railway signal IQ recordings | samples | LBJ | [#153](https://github.com/adswill/OnAir/issues/153) |
| [zanco48](https://github.com/zanco48) | AIS plotting and RSP2 DVB-T report | bug report | AIS; DVB | [#87](https://github.com/adswill/OnAir/issues/87), [#101](https://github.com/adswill/OnAir/issues/101) |

## By mode

- **5G NR cells**: gmg2719
- **ACARS**: A2ureIm4la, IZ5RZR
- **ADS-B**: A2ureIm4la, DL9DAK, Shadowman
- **AIS**: f0cyf, zanco48
- **ATSC**: 0xVANISHED
- **ATSC 3.0**: F5OEO
- **Airband**: Axpelle, catheno
- **Analog TV**: BSDnux, ea4gja, F5OEO
- **CDR**: hgxqk, RedmiNote5User, xxj737, YZYMC
- **Cell towers**: gmg2719, Mikki / YRPÄ
- **DAB / DAB+**: dj1fti, F5OEO, lakisfm1, Pacjunk, pbranly, ranaud80, ryanginn
- **DECT cordless phones**: bithunter-99
- **DMR**: drfruct
- **DTMB**: gmg2719, stdevPavelmc, xpwina, xxj737, YZYMC
- **DVB**: A2ureIm4la, ar8001, Debegege, dhihacker, drelectro, F5OEO, zanco48
- **DVB-C**: F5OEO, grue74
- **DVB-S/S2**: Arclamp (CasualArclamp), DG9BFC, DL5BCA, dominik0815, F5OEO
- **FM**: A2ureIm4la, ffishman, lakisfm1, RedmiNote5User
- **FT8**: drelectro
- **FreeDV**: ffishman
- **GNSS**: WasyaTheWolf
- **GSM-R**: F5OEO
- **General receiver**: kevgfire
- **Globalstar**: WasyaTheWolf
- **HD Radio**: tsalisbury0
- **HFDL**: drelectro, f0cyf, ffishman, frank-1225
- **ISDB-T**: dhihacker, F5OEO
- **Ionospheric sounders**: niuniu1138
- **Iridium**: WasyaTheWolf
- **LBJ**: YZYMC
- **LoRa (raw)**: carlosjs
- **LoRa APRS**: peterdewitte
- **MAC**: BSDnux
- **MeshCore**: drelectro, ffishman, KvaKKK, WispAyr
- **Meshtastic**: f5gkw, ffishman, KvaKKK, WispAyr
- **Meteor-M LRPT**: PY2PLL
- **Mode Builder**: DME-3
- **NAVTEX**: Axpelle
- **Orbcomm**: f0cyf
- **P25**: drelectro
- **QO-100**: ic706mk2, WolfgangSchreiner
- **TETRAPOL**: f5gkw
- **TV sat beacon decoder**: Arclamp (CasualArclamp)
- **Time signals**: michaelgeorge001, tortyl
- **VDL2**: IZ5RZR
- **VDL4**: f0cyf
- **Weather fax**: Axpelle
- **Wi-Fi HaLow (802.11ah)**: tsalisbury0
- **Wi-SUN FAN**: drelectro

## By area

- **Android**: hgxqk
- **Build**: drelectro, fe8769a, YZYMC
- **Credits**: DickvanNoort
- **Docker**: sbuchweitz
- **Interface**: A2ureIm4la, andriux26, drelectro, grahampaull, Jamiar, nature.van, peterdewitte, richt314, Sunnnset5, Tech500, Ugo300, unilabs-exp, Voyager-III
- **Languages**: andriux26, BI8SYN, ffishman, PU2SWX
- **Memory**: Ken W4KRR
- **Network radios**: andriux26, Chercheur, DG9BFC, drelectro, F5OEO, ffishman, MUSTARDTIGERFPV, nmaster2042, rlabs-oss, tortyl
- **Radios**: bellx2, drelectro, ea4hwg, iu0azb, iu1mrd, mechanicalpotato, mrwish7, OM5NA, OnesAndZer0s, sugusllly, Topol (Pawel Topolski), in collaboration with AI, trebor-56
- **Satellite passes**: andriux26
- **Tools**: peterdewitte
- **Video input**: peterdewitte
- **Windows**: ffishman, gvanem, py2gn
- **macOS**: FleXoft, modemuser
