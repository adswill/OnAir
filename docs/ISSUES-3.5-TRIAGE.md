# Issues triage for 3.5 (2026-10-31)

Open issues on adswill/OnAir: 136. Read: every issue updated in the last 3 days with its comments, then the rest by last comment. Branch names are on `pub/`. Effort: S small, M medium, L large.

Summary: already fixed on a branch or in 3.4.0: see rows marked *already fixed*; fixed now on `fix35-issues2`: #393, #408/#371/#333, #389, #390, #372, #357, #405 (docs); needs hardware data: #398 #397 #367 #358 #340 #337 #339 #331 #332 #328 #249 #148; declined with reason: #384 (covered by Multi-VFO), #255 (no time code to decode).

| Issue | Type | Status | Effort | Title |
|---|---|---|---|---|
| #408 | bug | fixed now: Ch column back in the cell table (and the traffic dead-band fix on fix35-ci2) | S | Tetrapol no information |
| #407 | bug | already fixed on feat35-lane-c / fix35-issues (ELT-DT protocol 9) | - | ELT-DT location protocol 9 is not decoded: unknown type, no position, unstable H |
| #405 | feature | fixed now (docs): Web-888 runs the KiwiSDR server, add it as type KiwiSDR; needs hardware data to confirm | S | [Feature] Add support for Web-888 receivers |
| #404 | bug | already fixed on fix35-issues (FGB false accepts) | - | False FGB decodes on live 406 MHz traffic and cross-triggering from real SGB bur |
| #403 | bug | already fixed on feat35-lane-c / fix35-issues (distance_km) | - | Automation Rule Notification Bug |
| #401 | bug | already fixed on fix35-issues (RTL rate range up to 3.2 Msps) | - | [Bug] Incorrect data for NESDR SMArt v5 |
| #400 | feature | already fixed in 3.4.0 (Europe 2027 button, more channels) | - | VDL2 add more channels |
| #398 | feature | needs hardware data: dual AD9361 (2 RX / 2 TX) needs a second RX channel path and a board to test | L | pluto clone (ad9361) dual |
| #397 | feature | needs hardware data: SigMF recordings to be downloaded into tests/data (owner) | S | Real over-the-air SGB recordings: normal and self-test bursts |
| #394 | feature | already fixed in 3.4.0 (type and distance columns) | - | VDL2 type d'avions et distance |
| #393 | bug | fixed now: 3.4.0 fixed the carry, a frequency saved by 3.3.0 still won over the mode default; now dropped (test_freq_remember) | S | 3.3.0: ADS-B → Airband/VDL2/AM stays at 1090 MHz due to satellite frequency carr |
| #390 | bug+feature | fixed now: image no longer restarts at 1500 lines (8000). Stop tone not detected and a slip editor need recordings | S | Feature Request: for existing Weather HF Fax |
| #389 | feature | fixed now: Calls table filter by type (Show: voice only, hide control ...); Save decoded messages on feat35-lane-c keeps carrier info | S | [Feature request] Log Carrier Information / Filter by Type |
| #386 | bug | already fixed on fix35-issues (mesh at 2.048 Msps) | - | Mesh (LoRa) on RTL-SDR: sample jumps break frames at 2.048 Msps (rtl_tcp and loc |
| #384 | feature | declined as a new mode: covered by Multi-VFO / multi-signal (docs/MULTI-VFO.md); reply explains | - | [ Feature Request ] Run Several Modes at the Same Time |
| #383 | feature | already fixed on fix35-issues (data folder) | - | Feature Request: Data Folder Selection |
| #379 | feature | known, queued (SondeHub excluded by owner) | M | [Feature request] More LoRa BWs, MRZ-N1 meteosondes, Qi wireless charger decoder |
| #376 | bug | already fixed on fix35-issues (mp3 lib path) | - | issue with mp3 lame dll for recording every update |
| #375 | feature | zh.txt is complete (3498 lines); needs native reviewers for wording, help welcome | M | Feature Request: Improved Chinese Language Support |
| #374 | bug | known, USRP TX frequency (queued) | M | Bug Report: Incorrect USRP Transmit Frequency |
| #373 | bug | known, USRP TX mode switching / CTCSS (queued) | M | Bug Report: USRP Transmit Mode Switching and NFM CTCSS Tone Selection |
| #372 | feature | fixed now: threads=N in the settings file (and DECT2_THREADS); row for the Settings window to follow | S | he program can be customized to set the number of threads and the amount of memo |
| #371 | bug | fixed now: same as #408 (Ch column) | S | Tetrapol Ch |
| #370 | question | docs/CODECS.md and the ? help cover it; reply | - | Codecs |
| #368 | feature | answered by the owner, waiting for the reporter | - | GW Notes |
| #367 | bug | needs hardware data: which Pluto firmware and which list; min rate is 2.048 Msps only with low-rate FIR on | M | low sample rate on pluto via nw |
| #365 | bug | already fixed in 3.4.0 (SSE2 clip) | - | DVB-T x86: SSE2 quantise() misses the ±limit clip for the 8-bit Viterbi — real m |
| #358 | bug | needs hardware data: CI-V address / rig model and mic permission details; not reproducible without the rig | M | ICOM-7300 & Hamlib |
| #357 | bug | fixed now: firmware structure check accepts the FX2 scratch RAM at 0xE000; the exact hash check still guards | S | IC-R8600 |
| #350 | bug | already fixed in 3.3.0 / 3.4.0 (GB2312, scrolling name) | - | FM RDS: Chinese characters in PS/RadioText are displayed as "?" — non-ASCII (GBK |
| #348 | bug | already fixed on feat35-lane-g (P25 control channel hunt) | - | P25 trunk following (3.2.0, Windows): follower leaves the control channel, cycle |
| #344 | feature | already shipped in 3.4 (text select/copy, test_text_select); reply | - | [Feature request] being able to select text, copy to clipboard |
| #341 | bug | already fixed in 3.4.0 (SSE2 Viterbi clip); waiting for reporter | - | Still decoding issue with DVB  (v.3.2.0) |
| #340 | bug | needs hardware data: the bundled librtlsdr is rtl-sdr-blog aed0ea1 (V4L commit); a check on a V4L is needed | M | RTL-SDR Blog V4L support: driver bundled with install uses 28.8 MHz clock instea |
| #339 | bug | needs hardware data: reporter says 3.2.0 worked, 3.3.0 DVB-T not working (viterbi fix is in 3.4.0) | M | Crashes as soon as  switch to TV |
| #338 | feature | already fixed in 3.4.0 (carry only between LNB modes) | - | Request: Keep selected frequency when possible |
| #337 | bug | needs hardware data: same driver question as #340 | M | RTL-SDR Blog V4L: no reception at all with OnAir-bundled driver; with official r |
| #336 | bug | already fixed in 3.3.0 (reporter confirmed), can be closed | - | Language set to 'same as system' the UI fonts unreadable in linux of use chinese |
| #335 | bug | known, DMR LOCN (queued) | M | DMR GPS position reports (LRRP and LOCN) apparently not decoded: nothing shown i |
| #333 | bug | fixed now: same as #408; traffic info kept by the dead-band fix on fix35-ci2 | S | Tetrapol: no information |
| #332 | bug | needs hardware data: crash gone, audio noise on DRM sample: xHE-AAC path | M | [Bug Report] DRM Mode Crash |
| #331 | bug | needs hardware data: libfdk-aac refuses the xHE-AAC 32 kHz mono stream; needs the IQ | M | DRM audio stuttering |
| #328 | bug | needs hardware data: claim key and selection use the position; a debug log with two same-serial dongles is needed | M | Bug: multiple receivers of the same type seemingly not possible |
| #324 | bug | already fixed in 3.2.0 / 3.4.0; non-ASCII folder start-up crash noted | - | DVB Crash since 3.1.0 |
| #321 | bug | answered by the owner, waiting for the reporter | - | Incorrect tuner rate for remote receivers |
| #320 | feature | answered by the owner, waiting for the reporter | - | TX panel colours ar very difficult to read |
| #319 | bug | answered by the owner, waiting for the reporter | - | App hangs after prolonged DTMB reception, GUI completely freezes when clicking " |
| #317 | feature | answered by the owner, waiting for the reporter | - | Hope to add NFM mode |
| #316 | bug | answered by the owner, waiting for the reporter | - | PlutoSky R2 – CMA allocation errors at 10 MS/s |
| #313 | bug | answered by the owner, waiting for the reporter | - | ISDB-T: one-seg service does not play when the PAT is missing (layer B below thr |
| #310 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | analog tv |
| #309 | feature | answered by the owner, waiting for the reporter | - | VDL2 log export |
| #308 | feature | answered by the owner, waiting for the reporter | - | PSK demodulator |
| #303 | feature | answered by the owner, waiting for the reporter | - | Request. in the VDES mode. |
| #301 | bug | needs a GUI check: General receiver centre button behaves differently from FT8 | M | Two Bugs Found |
| #300 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | dmr sms |
| #299 | feature | answered by the owner, waiting for the reporter | - | Some confusions |
| #297 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Some bugs about v3.0 |
| #296 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | waterfall |
| #291 | bug | answered by the owner, waiting for the reporter | - | Issue with 'setvbuf()' |
| #284 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | No waterfall under Debian Trixie on raspberry CM5 |
| #283 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | OnAir silently exiting on lauch under Debian Trixie on Raspberry CM5 |
| #281 | feature | answered by the owner, waiting for the reporter | - | Additional functionality for Iridium |
| #279 | bug | answered by the owner, waiting for the reporter | - | some codecs cannot be found when the app is installed in a non default location |
| #277 | bug | answered by the owner, waiting for the reporter | - | BUG:Analog TV transmission is slow |
| #275 | bug | answered by the owner, waiting for the reporter | - | BUG:CDR Decode Failed |
| #274 | feature | answered by the owner, waiting for the reporter | - | Requesting variable bandwidth |
| #271 | feature | answered by the owner, waiting for the reporter | - | Ideas regarding adding NFC decoding functionality |
| #270 | bug | answered by the owner, waiting for the reporter | - | Waterfall misalignment |
| #269 | feature | answered by the owner, waiting for the reporter | - | Feature Request: frequency detection and capcode detection functions for pagers |
| #266 | feature | answered by the owner, waiting for the reporter | - | About how to activate xHE-AAC |
| #264 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Feature Request: Noise Blanker |
| #258 | bug | needs a GUI check: aircraft icons in 3.3.0, 403 error on the map tiles | M | Feature Request from V0.29 |
| #257 | feature | answered by the owner, waiting for the reporter | - | KIWI SDR "radio stopped sending samples..." |
| #256 | feature | answered by the owner, waiting for the reporter | - | REQ : Decoder ARGOS Meteo satellite status |
| #255 | feature | declined for now: RWM sends no time code; showing calibrated time from the system clock is a small follow-up | S | Feature request: RWM decoder |
| #254 | feature | answered by the owner, waiting for the reporter | - | DTMB radio broadcasting is very choppy |
| #252 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Enable Dmr audio |
| #251 | feature | needs hardware data: Elad Duo native driver; sdroxide shows it is possible | L | Utilizzo diretto con Elad Duo? |
| #249 | bug | needs hardware data: DAB transmitter list, 30 s IQ link given (dropbox) for the owner to download | M | NO DECODE OF TRANSMITTER LIST IN DAB |
| #248 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Docs: Network radios: rtl_tcp, SpyServer, SoapyRemote and more |
| #247 | bug | answered by the owner, waiting for the reporter | - | DRM broadcast, xhe-aac cannot be decoded |
| #246 | bug | answered by the owner, waiting for the reporter | - | Waterfall not refreshing on its own current 0.8.2 version |
| #245 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Hope to add our radio driver. |
| #243 | feature | answered by the owner, waiting for the reporter | - | Feature request: Waterfall better resolution |
| #242 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Feature request (UI): detachable docking panes |
| #241 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Request for a New Mode: Codec Audio Tetrapol Mode |
| #239 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | [Feature Request] Analogue TV |
| #238 | bug | answered by the owner, waiting for the reporter | - | Bug: Changing frequency after zooming resets zoom level |
| #237 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Feature request: Control waterfall speed |
| #236 | feature | answered by the owner, waiting for the reporter | - | Feature request: Better usability for ultra-wide-band SDR's |
| #235 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | [Feature Request] Aircraft information on full screen map |
| #233 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Feature request: more advanced sdr configuration options |
| #229 | feature | answered by the owner, waiting for the reporter | - | Decoder request LORa Satellite |
| #228 | bug | answered by the owner, waiting for the reporter | - | MacOS SoapyRemote hangs OnAir till Force Quit |
| #226 | bug | answered by the owner, waiting for the reporter | - | Onair software cannot run |
| #225 | feature | answered by the owner, waiting for the reporter | - | [Feature Request]Add Unicode support to the Hellschreiber modulator |
| #223 | feature | answered by the owner, waiting for the reporter | - | [Feature request] Wider bandwidth for the Cospas beacon decoder and a reduction  |
| #219 | feature | answered by the owner, waiting for the reporter | - | SDRPlay RSP1 weird tuning frequency on click |
| #218 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Mesh (and other modes): signal time or the computer's clock for packet times? |
| #216 | feature | answered by the owner, waiting for the reporter | - | [Feature request] Add HF ham Pactor 1 to 4 support |
| #215 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Automatic search and configuration bricks unattended devices |
| #214 | feature | answered by the owner, waiting for the reporter | - | Hope to add decoders for China's GTH6/CF6 sounding balloons and SARSAT, along wi |
| #213 | feature | answered by the owner, waiting for the reporter | - | Feature requests: FM audio options, C-QUAM AM stereo, VHF-DSC, and satellite/TET |
| #211 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | SELCAL |
| #210 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | OnAir 0.2.7 (and 0.2.6) crash when using DAB+ on RTL-SDR v3 dongle |
| #201 | bug | answered by the owner, waiting for the reporter | - | Android app crashing upon starting SDR stream |
| #192 | feature | answered by the owner, waiting for the reporter | - | Morse Micro HaLow Networks |
| #189 | feature | answered by the owner, waiting for the reporter | - | Globalstar frequency labeling, GNSS sync drops, Iridium features, and CDR DRA+ |
| #184 | bug | answered by the owner, waiting for the reporter | - | v0.2.7 DTMB 506 and 514 works better than v0.2.6, but 690 cannot decode anything |
| #183 | feature | answered by the owner, waiting for the reporter | - | Feature Request: Add fallback mirror/proxy for update check and asset download f |
| #181 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Orbcomm pas de positions |
| #180 | bug | answered by the owner, waiting for the reporter | - | Astrometa DVB-T/2 nok for TV |
| #178 | bug | answered by the owner, waiting for the reporter | - | Bug: Incorrect display. Please change the label for DVB-T2 mux to DTMB. |
| #176 | feature | answered by the owner, waiting for the reporter | - | **Feature Request: Add MHEG-5 data reading support for DTMB digital TV** |
| #169 | feature | answered by the owner, waiting for the reporter | - | [Feature request] lime mini SDR add to radio list |
| #167 | feature | answered by the owner, waiting for the reporter | - | Enhanced WiFi HaLow |
| #166 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Crash when clicking on services in video decoders. |
| #165 | feature | answered by the owner, waiting for the reporter | - | HD Radio Decoding/Encoding |
| #164 | feature | answered by the owner, waiting for the reporter | - | LTE+NR Scanning |
| #163 | bug | answered by the owner, waiting for the reporter | - | DVB-T / DVB-T2 not locked on AirSpy Mini |
| #148 | bug | needs hardware data: airband scan finds frequencies but no audio | M | Adding channel to airband does not save |
| #139 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Feature: Alternate Touch-Friendly Interface? |
| #134 | bug | answered by the owner, waiting for the reporter | - | Instructions for posting in the issues tab! |
| #122 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Feature request: I hope there can be an Android build, which is convenient for m |
| #121 | feature | answered by the owner, waiting for the reporter | - | [Feature Request]Proposals for adding DRA+ decoding support for CDR broadcasts,  |
| #104 | bug | answered by the owner, waiting for the reporter | - | 'assert()' in '_DEBUG'-mode |
| #103 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Weak WEFAX signals |
| #96 | bug | answered by the owner, waiting for the reporter | - | Bug Report: DVB-T/T2 8 MHz Signals Not Detected or Locked with PlutoSDR over IP |
| #88 | feature | answered by the owner, waiting for the reporter | - | DTMB video and audio always frozen or jitter even C/N is very high and constella |
| #84 | feature | answered by the owner, waiting for the reporter | - | SoapySDR over lan |
| #79 | bug | answered by the owner, waiting for the reporter | - | Meteor M2-3 LRPT fail to decode. |
| #78 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | Program closes when AirSpy Discovery is stopped |
| #73 | feature | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | [Feature request] Add more analogue television systems and more mechanical modes |
| #66 | bug | reporter replied; not changed in this pass (older item, answered earlier, see the thread) | - | HFDL ne fonctionne pas |
| #39 | feature | answered by the owner, waiting for the reporter | - | CUDA/GPU DTMB Decoder |
