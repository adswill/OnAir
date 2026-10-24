# Open feature requests on GitHub, 2026-10-24 (against v3.0.0)

Source: `gh issue list --repo adswill/OnAir --state open` (about 100 open), v3.0.0 tree, docs/CHANGELOG-3.0.md,
docs/MODE-NUMBERS.md, docs/ISSUES-2026-10-23-features.md and ISSUES-2026-10-23-modes.md. Effort: S under 1 day of agent work, M a few days, L a week or more.
Rules applied: no patented codec shipped (user-installed plug-ins only), no decrypting without user keys, no reverse engineering of proprietary protocols, transmit only through Arm + licence + band-plan.
Issues in 3.0.0 and still open are waiting for the reporter to confirm, or for a real signal.

| issue | request | in 3.0? | effort | risk / needs | recommendation |
|---|---|---|---|---|---|
| 39 | CUDA/GPU DTMB decoder | partly: Metal (0.2.6), Vulkan LDPC on Linux (3.0, no real GPU tried) | M (CUDA), M (D3D) | needs NVIDIA / Windows GPU to test | 3.1 only if a tester appears; ask reporter to try Vulkan |
| 73 | more analogue TV (MAC, MUSE, HD-MAC, FM sat) | partly: NICAM, A2, D/D2-MAC FM; docs/ANALOGUE-TV.md | L | BTSC dbx is licensed; MUSE/HD-MAC need real signals | 3.1: C-MAC/B-MAC if hacktv recordings come; BTSC no |
| 85 | CSV resolution, metric ADS-B | yes (RBW list, labels, metric) | - | - | close after reporter confirms |
| 95 | FLEX Next | yes (ordinary FLEX, fixes in flexnext-030) | - | other bursts | close |
| 121 | DRA+ for CDR | yes (0.2.6; DTMB DRA add-on #266) | - | DRA is user-installed | close |
| 122 | Android | yes (0.2.6 APK, receive only) | M (OTG drivers Airspy/RSP) | separate Android repo | 3.1: OTG drivers |
| 137 | sound card IF input | yes (b74284bc0) | - | IC-7300 untested | close |
| 139 | touch UI | yes (View > Touch interface) | - | - | close |
| 164 | LTE/NR: filters, MFBI, multi-carrier NR, alerts | partly (SIB1 bands above 64, PLMN filters) | M | needs live LTE/NR captures | 3.1: NR multi-carrier, alert text |
| 165 | HD Radio decode/encode | decode yes; encode no | encode L | HDC patented + broadcast licence: user plug-in only behind Arm gate | decode: fix long names and logos (S); encode: no |
| 167 | HaLow 8/16 MHz | yes (2a62aa881, 758f6484d) | - | real capture | close |
| 169 | LimeSDR Mini | yes (LimeSuite, SoapyLMS7) | - | no hardware test | close |
| 176 | MHEG-5 on DTMB | partly (carousel viewer, scene renderer, experimental) | M | TVB stream link expired; needs owner/reporter recording | 3.1 when a sample arrives |
| 183 | China mirror for updates | partly (user-entered mirror) | S | built-in third-party proxy is a trust risk | no built-in default; add a documented list |
| 189 | Globalstar, GNSS drops, Iridium, DRA+ | yes except BeiDou | S | BeiDou needs a capture; AM stereo/AMSS question open | 3.1 with capture |
| 192 | Morse Micro HaLow | partly (6 Msps, 2 MHz sub-channel scan) | M | real Morse Micro capture; reporter says still no decode | 3.1, needs capture |
| 211 | SELCAL on HF | yes (91cd1fc53, voice channels too) | - | real SELCAL | close |
| 213 | FM audio, C-QUAM, VHF-DSC, GK-2A, TETRA voice | mostly yes; GK-2A no | M (GK-2A) | GK-2A needs a sample | 3.1 only with sample |
| 214 | GTH6/CF-06 sondes, SARSAT 1544, baseband out | partly (SARSAT search yes) | M | GTH6 format description + recordings missing | 3.1 with recordings |
| 216 | PACTOR 1-4 | PACTOR-I only | II-IV not possible | proprietary (SCS), no reverse engineering | no for II-IV |
| 218 | packet time: signal vs computer clock | partly (mesh, ADS-B) | M | design decision: one engine-wide block clock | 3.1 (owner decides design) |
| 223 | Cospas 100 kHz, false decodes | yes (ec1bf9292, 94e47accf) | - | - | close |
| 225 | Unicode Hellschreiber modulator | yes (UniHELL, hfdig_kbd_hell) | S (font download option promised) | - | close; font download 3.1 |
| 229 | LoRa satellites | yes (mode 201, FOSSASAT presets) | - | real pass | close |
| 233 | more SDR config (stages, device string, RTL AGC, RSP1A LNA) | partly (RTL AGC, decimate, Soapy string) | S | real radios | 3.1: expose all advertised gains |
| 236 | ultra-wide-band usability | partly (ACARS whole capture, 15 modes whole span) | M | VDL2/HFDL whole span, second instance | 3.1: whole span for VDL2/HFDL |
| 241 | Tetrapol voice codec | plug-in slot only | - | RP-CELP never shipped; clear calls only | no more work; docs |
| 242 | detachable docking panes | partly (docking in window) | L | needs ImGui multi-viewport swap, affects every window | 3.1 as branch, decide after test |
| 245 | vendor offers radio driver | no | M | hardware offer, owner talks to vendor | owner decision |
| 248 | network radios docs, TCI | docs yes; TCI no | M (TCI) | needs ExpertSDR3 to test | 3.1 if tester |
| 251 | Elad Duo direct | no | L | proprietary ExtIO/IQ; works as Hamlib + sound card | no (document workaround) |
| 255 | RWM decoder | yes (5d5a7aace) | - | on-air check | close |
| 256 | Argos | yes (mode 152) | - | recording | close |
| 258 | Tetrapol 10 kHz | yes | - | - | close |
| 264 | noise blanker | yes | - | real HF QRM | close |
| 268 | DVB-C guide language | yes | - | Finnish DVB-C | close |
| 269 | pager freq + capcode detection | yes (Capcodes view, watch list) | - | - | close |
| 271 | NFC | yes (mode 148) | - | real capture | close |
| 274 | variable bandwidth 0-2 MHz | yes (RATE box) | - | asked what else is meant | wait for reply |
| 280 | pt-BR / pt-PT | yes | - | native review | close |
| 281 | Iridium: voice, ACARS | yes (plug-in, ACARS, pagers) | - | real capture | close |
| 282 | BPSK mode + analyser | yes (PSK demodulator, mode 202) | - | experimental, no real signal | close; QPSK/8PSK present |
| 247, 266 | DRM xHE-AAC | partly (libfdk-aac found; status line) | S | codec user-installed; Windows load path unverified | 3.1 check on Windows |
