# My station and decode help (3.8)

Branch `feat38-station`. Everything here is new code; nothing existing was replaced.

## What was added

| Item | Where | Notes |
|---|---|---|
| My station | `core/station/station.*`, `app/panels/station_ui.cpp`, View > My station | Radios are added by themselves when found; an antenna is one click from a list of common ones, details under Advanced. |
| Mode list | `modeFit` (`mode_info.cpp`), mode card | Once an antenna or a second radio is entered, the list judges the whole station and says what is missing. Without that, the old check of the selected radio applies. Never blocks. |
| Automatic radio | `stationAutoPick`, `pickRadio` | Selecting a mode moves to another free radio only if the selected one is clearly worse (cannot do the mode, or lacks what another has). A radio that works is left alone. Start moves only off a radio held by another receiver. Off switch under Advanced. |
| Radio sharing | `assignRadios`, Advanced button | Optimal assignment (dynamic programming over radios): as many modes Ready as possible, then Partial; an impossible mode takes no radio from a possible one. |
| Search everything heard | `core/station/heard.*`, `app/panels/heard_ui.cpp` | Call sign, ICAO, MMSI, frequency, MHz range or a word, over `msgdb`, grouped per station. A live position comes from the map screens' `TrackHub`s (`liveFix`). |
| Several modes at once (#384) | `core/engine/wide_placed.*`, `WideSession::setOnlyManual`, Everything in view window | Signals placed at fixed frequencies with their own mode; they stop when the capture leaves them and come back. One button fits them all in one capture. |
| Decoder progress | `core/doctor/decode_progress.*`, `doctor_ui.cpp` | Signal, carrier, sync, error check, from each receiver's `state` and `blocksOk` / `blocksBad`. Modes that report neither show "not reported". |
| Doctor opens itself | `AutoOpen` | After 25 s of a live radio getting nowhere, once per mode and run, and only for modes that report a state. Off switch under Advanced. |
| Failed-signal clip | `writeClip`, `doctor_ui.cpp` | The doctor's quarter-second capture as SigMF (`ci8`), no location. Needs a tick for that one clip, and the tick resets after each save. |
| CI hook | `tools/signal_library.py`, `tests/real/signal_library.txt`, test `signal_library` | Decodes listed recordings with `dect2cli`. `$ONAIR_SIGNAL_LIB` adds a folder with its own manifest. |
| Antenna and gain coach | `coach()`, doctor window | Advice from the doctor's numbers and My station. |
| Surprise me | `surprise()`, My station, command palette | A random receivable mode, those not heard yet three times as likely. |
| Rare catch alerts | `RareWatcher`, `heard_ui.cpp` | Emergency squawks, AIS-SART / MOB / EPIRB, and the first message ever of a mode. Off switch under Advanced. |
| Dossier | `dossier()`, Dossier button in Search | Country (ICAO block, MMSI MID), registration where the address fixes it, modes, frequencies, last position, latest messages. |
| Plain-English messages | `plainEnglish()`, Dossier | Only what a fixed rule can say for certain: emergency squawks, AIS navigational status (ITU-R M.1371), MMSI kind and country, ICAO country. Says nothing otherwise. |

## Validation

Unit tests, all passing locally against the prebuilt 3.7 libraries:

- `station`: verdicts, band edges, busy and unplugged radios, the automatic choice, assignment (up to 12 radios and 12 modes), settings text, capture planning.
- `heard`: the query parser, grouping over an in-memory database, live positions.
- `wide_placed`: a POCSAG carrier and an air-band AM station placed on one 2.4 Msps capture decode together (16 pages, the 700 Hz tone in the sound), a DMR carrier not placed stays off, and the placed list follows a retune. **Simulated signals only.**
- `decode_help`: stages, automatic opening, clip files, coach.
- `station_fun`: pick weights (3.00 to 1 over 40 000 seeds), rare catch rules, dossier, plain English.
- `signal_library`: the ADS-B excerpt (dump1090 `modes1`, a real recording) decodes through `dect2cli`; a wrong result fails, a missing file is reported, an empty library is skipped.

The app files were checked with `clang++ -fsyntax-only` only: **no full app build and no GUI run was done on this branch** (overnight limits). The windows have not been seen on screen.

## Not done

- **"Send" does not upload.** The app has no upload path and no endpoint was specified, so inventing one was ruled out. The clip is saved to `failed_signals/` in the data folder and the user attaches it to an issue or the sample library.
- **#384, partly.** Separate audio outputs per placed signal and two modes on the same frequency are not done. The placed signals use the existing `WsKind` decoders, so a placed signal can be any of those, not any mode in the list.
- **Automatic antenna choice is advice only.** OnAir cannot switch a physical antenna; the mode card names the pair to use.
- **Decoder progress covers the receivers wired into the table** in `doctor_ui.cpp` (40 or so, those with a `state` field). Others show "not reported" and do not open the doctor.
- New interface strings are not translated yet.
