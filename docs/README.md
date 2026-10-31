# OnAir documentation index

The user guides and notes that live in the repository. The same text is on the [wiki](https://github.com/adswill/OnAir/wiki) and at [on-air.dev/docs](https://on-air.dev/docs/). The main README is [../README.md](../README.md); radios are in [../DEVICES.md](../DEVICES.md); the code layout is in [../ARCHITECTURE.md](../ARCHITECTURE.md).

| File | Topic | What it covers |
|---|---|---|
| [IPTV.md](IPTV.md) | Internet TV (IPTV) | Plays playlists (M3U) and single streams (HLS, MPEG-TS over HTTP, UDP, RTP) with a channel list, favourites and an XMLTV guide. Ships no channels, no DRM. How https works in the FFmpeg-free fetcher. |
| [asterix.md](asterix.md) | ASTERIX surveillance data | Receive only. The mode ASTERIX decodes CAT 021, 048 and 062 radar and ADS-B data from UDP or a file into a track table and a map. |
| [audio-cleanup.md](audio-cleanup.md) | Audio cleanup | A chain that cleans the sound you hear. |
| [auto-gain.md](auto-gain.md) | Auto gain (AGC) | The AGC keeps a radio's front end at the best operating point for the active mode. |
| [band-plans.md](band-plans.md) | Band plans | A band plan is the channel raster and the allocations of a country: |
| [band-survey.md](band-survey.md) | Band survey | A band survey keeps the sweeps of one frequency range over time, so you can see which signals are always there, which parts of the band are quiet, and what changed since  |
| [benchmark-modes-0.2.7.md](benchmark-modes-0.2.7.md) | Per-mode benchmark, 0.2.7 baseline | Made with `bench_modes --seconds 10 --rates both --out ...` (tests/bench/bench_modes.cpp) on a Mac under heavy load from other jobs, so only CPU time is used: |
| [codar-research.md](codar-research.md) | CODAR / HF ocean radar: what is publicly documented (research note, 2026-10-13) | Sources: |
| [CODECS.md](CODECS.md) | Voice and audio codecs | > **DMR, P25, NXDN, dPMR and D-STAR voice is silent until you install mbelib.** The calls, IDs and data are shown without it. |
| [credits-for-website.md](credits-for-website.md) | Credits and licences | Paste this block into the website and the wiki. |
| [datawell-research.md](datawell-research.md) | Datawell Waverider buoy HF link: what is public, what was found (research note, 2026-10-13) | Sources (all fetched and read; downloaded into a scratch directory, nothing executed): |
| [DECT.md](DECT.md) | DECT bands | OnAir listens to DECT base-station beacons (identity, system information, bursts). |
| [doppler-orbit.md](doppler-orbit.md) | Orbit refinement from measured Doppler | The Passes window has an **Orbit from Doppler** section. |
| [eczas.md](eczas.md) | e-Czas (Poland, 225 kHz long wave) | e-Czas ("e-CzasPL") is the official time service of the Polish Central Office of Measures (GUM). |
| [fpv.md](fpv.md) | FPV video (analog) | The mode **FPV video (analog)** (TV group) receives the analog video links of drones and FPV goggles: |
| [frequency-db.md](frequency-db.md) | Frequency database | A built-in, searchable list of well-known frequencies for every mode (View > Frequencies, `Ctrl+Shift+F`, or the **Known frequencies** drop-down of the Tuner panel). |
| [gnss-map.md](gnss-map.md) | GNSS satellites on the map | Where the satellites are comes from the navigation message the receiver decodes (parity/CRC checked, complete and consistent sets only), computed at the |
| [gps-jamming-map.md](gps-jamming-map.md) | GPS interference map | ADS-B aircraft broadcast how good their own GNSS position is. |
| [headless-server.md](headless-server.md) | Headless server (onair-server) | `onair-server` is OnAir without a window. |
| [RASPBERRY_PI.md](RASPBERRY_PI.md) | OnAir on a Raspberry Pi | Pi 4/Pi 5 install, udev, headless use, which modes fit (untested on real hardware). |
| [hydrogen-line.md](hydrogen-line.md) | Hydrogen line toolkit (Radio astronomy mode) | Everything here works on the integrated spectrum of the Radio astronomy mode (21 cm, 1420.405751 MHz). |
| [kiwisdr.md](kiwisdr.md) | KiwiSDR (network HF receiver) | A KiwiSDR is a software defined receiver that covers 0 to 30 MHz and is reached over the internet or a local network. |
| [lbj.md](lbj.md) | LBJ (Chinese railway train approach warning) | Receive only. |
| [LICENSE-NOTICES-BINARIES.md](LICENSE-NOTICES-BINARIES.md) | Licences of the libraries inside the OnAir packages | OnAir is GPL-3.0-or-later (file LICENSE; source: |
| [live-rooms.md](live-rooms.md) | Live rooms | Server > Go live streams a receiver's waterfall (and, if chosen, decoded ACARS text) to an audience on the web, with chat. |
| [messages.md](messages.md) | Messages: the decoded-message database | View > Messages opens one searchable store of everything the decoders heard: |
| [mscatter.md](mscatter.md) | Meteor scatter | The mode **Meteor scatter** (Weather & science group) counts meteors by radio. |
| [mt63.md](mt63.md) | MT63 | MT63 is the keyboard-to-keyboard mode of Pawel Jalocha (SP9VRC) that is built to survive fading, interference and a badly tuned dial: |
| [multi-sdr.md](multi-sdr.md) | Several radios at once (multi-SDR) | Warning shown when a receiver is added: |
| [noise-remover.md](noise-remover.md) | Neural noise remover (Listen) | Removes steady background noise from speech with RNNoise (Xiph.Org, BSD-3-Clause; vendored in |
| [DIVERSITY.md](DIVERSITY.md) | Diversity pair (two radios, one better signal) | Two radios on one frequency combined by selection or maximal-ratio combining for weak narrowband signals; off until a pair is made; start-up alignment, delay / offset / drift tracking, tested SNR gains and limits. |
| [nrsp-st.md](nrsp-st.md) | SDRplay nRSP-ST (network) | Source: |
| [optimising-a-mode.md](optimising-a-mode.md) | Optimising a mode | The rule: |
| [PACKS.md](PACKS.md) | Data packs (work without internet) | View > Data packs lists data you can download once and then use offline: |
| [pass-scheduler.md](pass-scheduler.md) | Pass scheduler | The Passes window (View > Passes) can take satellite passes by itself: |
| [passive-radar.md](passive-radar.md) | Passive radar (View > Passive radar) | Detects aircraft from the echoes of a transmitter that already exists (an FM or DVB-T station), with no transmitter of your own. |
| [PATHWAY.md](PATHWAY.md) | PATHWAY.md | New mode fpv (family 101, active 102, std 103): |
| [plugins.md](plugins.md) | Plugins | Plugins let other people add decoders ("modes") and actions to OnAir without rebuilding it. |
| [position-estimation.md](position-estimation.md) | Aircraft position estimation | One estimator serves ADS-B, UAT, ACARS, VDL2 and HFDL (`core/include/dect2/track/`, `core/src/track/`, test `tests/track/test_track.cpp`). |
| [protocol-lab.md](protocol-lab.md) | Protocol lab | View > Protocol lab... |
| [MODE-BUILDER.md](MODE-BUILDER.md) | Mode Builder: decoders from a JSON definition (format shared with the Signal Lab) | Tools > Mode Builder... |
| [rf-camera.md](rf-camera.md) | RF camera | The RF camera lays the direction finder's bearing spectrum over a picture as a heat map, so you see roughly where in a scene a signal comes from. |
| [rfi-hunter.md](rfi-hunter.md) | Interference hunter (View > Interference hunter) | Watches a band for new interference, guesses what kind of source it is, and helps you walk to it with a loud / quiet meter. |
| [sbas.md](sbas.md) | SBAS (L1 augmentation messages) | Receive only. The mode SBAS receives the geostationary augmentation satellites on 1575.42 MHz and shows what they send; nothing is applied. |
| [same.md](same.md) | NOAA Weather Radio SAME (EAS alerts, US and Canada) | Receive only. |
| [sarsat.md](sarsat.md) | COSPAS-SARSAT 406 MHz distress beacons | Receive only. |
| [scanner.md](scanner.md) | Sweep scanner | A fast wideband sweep with a noise-floor measurement: |
| [self-calibration.md](self-calibration.md) | Receiver frequency self-calibration | A cheap radio's oscillator is off by tens of ppm: |
| [SOUND-CARD.md](SOUND-CARD.md) | Sound card input (audio device) | Pick "Sound card: |
| [STATION_HEALTH.md](STATION_HEALTH.md) | Station health | View > Station health. |
| [threading.md](threading.md) | Threading in OnAir (0.2.7) | Survey of how threads are used, the shared framework (`core/include/dect2/common/threadpool.h`, `queue.h`, `stage_pipeline.h`), where it is applied and where it is delibe |
| [time-machine.md](time-machine.md) | Rewind (time machine) | The "Rewind" button in the diagnostics row keeps the last seconds of the radio's raw samples in memory, so the signal from just *before* |
| [tones.md](tones.md) | Tone decoders (CTCSS, DCS, DTMF, selcall, SELCAL) | The mode **Tone decoders (CTCSS, DCS, DTMF, selcall)** (digital voice and two-way radio group) listens to the audio of a narrow-FM channel, or to a sound card, and |
| [TRANSCRIBE.md](TRANSCRIBE.md) | Live transcripts: airband and marine voice as text | > **Off until you turn it on, and no speech model is included.** OnAir downloads one only when you click, or reads a file you put in the models folder. |
| [TRANSMIT.md](TRANSMIT.md) | Transmit | **You are responsible.** OnAir limits nothing: |
| [trunking.md](trunking.md) | Trunking follower (P25, DMR Tier III) | Receive only. |
| [TV-CHANNELS.md](TV-CHANNELS.md) | TV channels | The Channel selector of every TV mode: the channel plan of a country for DVB-T/T2, ATSC, ISDB-T, DTMB, DVB-C and analog, with sources. |
| [tv-recorder.md](tv-recorder.md) | TV recorder: programme guide, recordings, timeshift, subtitles | Works on the DVB transport stream that OnAir already decodes (DVB-T/T2, DVB-C, DVB-S/S2, ISDB-T, DTMB and the others that deliver an MPEG-TS). |
| [UPDATE-MIRROR.md](UPDATE-MIRROR.md) | Updating when GitHub is blocked or slow | OnAir normally asks `api.github.com` for the newest release and downloads the package from `github.com`. |
| [watchlists.md](watchlists.md) | Watchlists and alerts | View > Watchlists. |
| [wmbus.md](wmbus.md) | Wireless M-Bus (EN 13757-4) | The mode **Wireless M-Bus (meters)** (IoT & sensors group) lists the utility meters (water, heat, gas, electricity, heat cost allocators) that send their |
| [zwave.md](zwave.md) | Z-Wave (ITU-T G.9959) | A receive-only monitor for Z-Wave home automation networks on the sub-GHz bands. |
| [esubghz.md](esubghz.md) | Flipper Zero Sub-GHz chat | Chat with Flipper Zero devices (Enhanced Sub-GHz Chat app), plain or AES-256-GCM encrypted, receive and send. |
| [roshf.md](roshf.md) | ROS (HF) detector | Detects the ROS weak-signal HF mode and names its sub-mode; text decoding partial (needs a public spec). |
| [ssdv.md](ssdv.md) | SSDV pictures | Balloon and satellite pictures from SSDV packets (FSK / RTTY-framed), built up while they arrive. |
| [nrf24.md](nrf24.md) | nRF24L01 | Enhanced ShockBurst frames on one 2.4 GHz channel, 250 kbit/s / 1 / 2 Mbit/s, address sniffer (needs a 2.4 GHz SDR; needs a user test). |
| [INMARSAT.md](INMARSAT.md) | Inmarsat (all carriers) | Every Aero P and Inmarsat-C carrier of one wide capture at once; C voice not decoded (needs a user test). |
| [ebicab.md](ebicab.md) | EBICab / Eurobalise | Balise up-link telegrams per SUBSET-036 (needs a user test); EBICab formats not public. |

## Added in 3.0 (not in the table above yet)

| File | Topic |
|---|---|
| [ANTENNA.md](ANTENNA.md) | Antenna designer |
| [AUDIO.md](AUDIO.md) | Audio control |
| [AUTOGAIN.md](AUTOGAIN.md) | Auto gain |
| [BACKUP.md](BACKUP.md) | Backup and restore (Tools > Backup and restore) |
| [DAB-AUDIT.md](DAB-AUDIT.md) | DAB / DAB+ receiver audit: audio continuity (0.3.0) |
| [DRM-AUDIT.md](DRM-AUDIT.md) | DRM receiver audit: smooth audio (0.3.0) |
| [DSC.md](DSC.md) | Marine DSC (ITU-R M.493) |
| [DXPATROL.md](DXPATROL.md) | DXpatrol SDR |
| [GUI-AUDIT.md](GUI-AUDIT.md) | GUI audit (0.3.0, branch gui-audit-030) |
| [HAM_PLAN.md](HAM_PLAN.md) | Ham operation plan (goals section 7) |
| [HORUS.md](HORUS.md) | Horus balloons (Project Horus telemetry) |
| [I18N.md](I18N.md) | Interface translations |
| [IDENTIFIER.md](IDENTIFIER.md) | Signal identifier |
| [IMAGE-ENHANCE.md](IMAGE-ENHANCE.md) | Image enhancement |
| [IQ-FORMATS.md](IQ-FORMATS.md) | IQ file formats |
| [ISDBS.md](ISDBS.md) | ISDB-S (BS satellite television, Japan) |
| [KAAL-CODEC.md](KAAL-CODEC.md) | KAAL voice codec 4 (KV4) |
| [KAAL-ENHANCE.md](KAAL-ENHANCE.md) | KAAL voice codec 4: decoder-side speech enhancer |
| [KAAL-NEXT.md](KAAL-NEXT.md) | KAAL next (frame version 4): four voice slots, fast chirp sync, better receiver |
| [KAAL-SLOTS.md](KAAL-SLOTS.md) | KAAL v4 voice slots: interface and bit budgets for the codec |
| [KEYS.md](KEYS.md) | Voice decryption keys |
| [LAN-SWARM.md](LAN-SWARM.md) | LAN swarm |
| [LORA_SATELLITES.md](LORA_SATELLITES.md) | LoRa satellites (mode "LoRa satellites", family 201) and LoRaWAN (all channels, family 200) |
| [MODE-NUMBERS.md](MODE-NUMBERS.md) | Mode numbers |
| [NETWORK_RADIOS.md](NETWORK_RADIOS.md) | Network radios: where they are saved, and recovering from a hang |
| [NFC.md](NFC.md) | NFC / RFID at 13.56 MHz |
| [NIXOS.md](NIXOS.md) | OnAir on NixOS and Nix |
| [ONAIR-DIRECTORY.md](ONAIR-DIRECTORY.md) | OnAir Web directory |
| [ONAIR-WEB-PROTOCOL.md](ONAIR-WEB-PROTOCOL.md) | OnAir Web protocol, version 1 |
| [ONAIR-WEB.md](ONAIR-WEB.md) | OnAir Web: share your radio with many listeners |
| [PACKET.md](PACKET.md) | APRS / Packet (AX.25) |
| [PAGER_CCIR.md](PAGER_CCIR.md) | CCIR paging in OnAir |
| [PERFORMANCE-DSP.md](PERFORMANCE-DSP.md) | Signal-processing cost (0.3.0 work, branch perf-dsp-030) |
| [PERFORMANCE-DVB.md](PERFORMANCE-DVB.md) | DVB family performance |
| [PERFORMANCE-POWER.md](PERFORMANCE-POWER.md) | Idle and background power use |
| [PERFORMANCE.md](PERFORMANCE.md) | Performance and power (0.3.0 work, branch perf-ui-030) |
| [PRESETS.md](PRESETS.md) | Per-mode presets |
| [RELEASE_PIPELINE_PLAN.md](RELEASE_PIPELINE_PLAN.md) | Release pipeline plan (goal: "release <version>" to public in 2 hours or less) |
| [REMOTE-API.md](REMOTE-API.md) | Remote control API |
| [ROS.md](ROS.md) | ROS: what is publicly known, and what OnAir does with it |
| [RTLSDR-V4L.md](RTLSDR-V4L.md) | RTL-SDR Blog V4 Lite (V4L) |
| [SATELLITE_AIS.md](SATELLITE_AIS.md) | Long-range and satellite AIS |
| [SIGNAL-LAB.md](SIGNAL-LAB.md) | Signal Lab |
| [SYNTH-WIDEBAND.md](SYNTH-WIDEBAND.md) | Synthetic wideband: several narrow radios as one wide radio |
| [TX-OPTIMISATION.md](TX-OPTIMISATION.md) | TX optimisation (0.3.0) |
| [ardop.md](ardop.md) | ARDOP (monitor) |
| [argos.md](argos.md) | Argos |
| [bletrk.md](bletrk.md) | BLE tracker detector |
| [cquam.md](cquam.md) | AM stereo (C-QUAM) |
| [dab-transmitters.md](dab-transmitters.md) | DAB transmitter lists (Map tab) |
| [direction-finding.md](direction-finding.md) | Direction finding (KrakenSDR / KerberosSDR style arrays) |
| [dvbs-decsat-parity.md](dvbs-decsat-parity.md) | DVB-S/S2 in OnAir against DecSAT |
| [dvbs.md](dvbs.md) | DVB-S, DVB-S2 and DVB-S2X in OnAir |
| [fast-scan.md](fast-scan.md) | Fast scan (Scanner view > Fast scan) |
| [fsq.md](fsq.md) | FSQ (Fast Simple QSO) |
| [goes-dcs.md](goes-dcs.md) | GOES / Meteosat DCS |
| [ifkp.md](ifkp.md) | IFKP (Incremental Frequency Keying Plus) |
| [marine-vhf.md](marine-vhf.md) | Marine VHF (channels 1 to 88, 156 to 162 MHz) |
| [modeac.md](modeac.md) | Mode A/C |
| [propagation.md](propagation.md) | Propagation (Tools > Propagation) |
| [qo100-auto.md](qo100-auto.md) | QO-100 narrowband: presets, auto decoder, conversation manager |
| [railway.md](railway.md) | LBJ, KLW and TDCS railway reception |
| [satbeacon.md](satbeacon.md) | TV sat beacon decoder |
| [sigfox.md](sigfox.md) | Sigfox |
| [voice-inversion.md](voice-inversion.md) | Voice inversion |

The release text is [CHANGELOG-3.0.md](CHANGELOG-3.0.md) (generated by `tools/release/gen_changelog.py`); the new strings for the translation pass are in [I18N-NEW-STRINGS-3.0.md](I18N-NEW-STRINGS-3.0.md).
