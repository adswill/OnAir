# Discord reports, 3.8 (branch feat38-discord)

## 1. Airspy HF+ / HF+ Discovery: "the gain slider does nothing", "OnAir keeps saying raise the gain"

Cause. The driver mapping was right: the slider (0 to 48) sets the attenuator in 6 dB steps (`set_hf_att` 8 .. 0), the preamp comes on in the top 6 dB (Preamp = Auto), and the radio's AGC stays off unless Radio AGC is set. A new test against the fake libairspyhf sweeps every slider position (0 to 48 in 1 dB steps, live retune) and checks each one reaches the attenuator, monotonic, with the preamp only above 42 and the AGC off.

What went wrong is the level. The HF+ firmware compensates the preamp in its gain chain (Airspy changelog R1.1: "no level change should be expected") and treats the attenuator as the HF gain distribution (R1.3), with its own digital gain behind it. So the output level in dBFS hardly follows the slider, and a normal HF+ level (often -60 to -80 dBFS) looked "low" to OnAir. Every level-based check then said "raise the gain", and the auto gain loops could push the attenuator to either end because the level never answered. An RTL-SDR's level follows its gain, which is why it behaved differently.

Changes:
- `setAdcLevelCompensated()` (core/source/gain.h), set for the HF+ from the toolbar next to `setAdcBits()`. While it is set, `classifyAdc()` never reports Low (status bar, mode screens), and the level-based "raise the gain" advice is not given (reception doctor, decode coaching, MCP diagnosis, next step line).
- SmartAgc (`GainModel::levelCompensated`): the least attenuation allowed (42 without the amp permission, 48 with it), down only on clipping; a gain that clipped stays out of reach for 5 minutes. New reason "least attenuation: this radio sets its own level".
- AutoGainController (`RadioProfile::levelCompensated`, the feat37-autogain code already in integ-38; there is no feat38-autogain branch): the headroom loop does not judge the HF+ by level; it climbs to the top of what is allowed, the quality guard can undo a step, clipping keeps its ceiling for 5 minutes, and the quick search starts at the top and its measured-quality pick holds until the next search. Its other Airspy assumptions (0..48 in 6 dB steps, preamp in the top 6 dB only with permission, 16 bits) are correct.
- The Radio AGC and gain help texts say what the slider does on this radio, that a low dBFS level is normal, that Airspy's own default is AGC on, and that on older firmware the attenuator acts only below 31 MHz.

Tests (local, standalone against the prebuilt libraries): test_agc (new: 6 levels x 5 start gains x amp allowed or not with a level that does not follow the gain, a front end that clips above 30 dB, the ADC status), test_autogain, test_native HF+ sweep: all pass. test_native has 4 failures in this local setup (rtl tone at 192 kHz, Blog V4 direct sampling, HF+ bias-tee listing), identical with the unmodified test: not from this branch. Simulated only: no HF+ was connected, and whether the firmware also scales the attenuator digitally is inferred from Airspy's changelog and the reports, not measured.

## 2. "Your name on the map" missing on the Raspberry Pi

Cause. There is no Pi build condition. The name editor was a submenu near the end of the Server menu (after up to six long toggles), and only while sending was on. On a small Pi screen the tall menu is cut at the bottom edge, and a submenu needs hover, which a touch screen does not give. The share toggles were visible, the name entry was not.

Fix (every platform, both the menu bar Server menu and the old interface's Server button): "Your name on the map: <name or anonymous>..." is now the second item, always shown, and opens a centred window that is never larger than the screen. Nothing is sent from it. Checked by compiling only (no app build tonight): not seen on a Pi screen.

## 3. DRM frequencies from drm.org (B26)

The schedule page (https://www.drm.org/drm-in-the-world/broadcast-schedule/, B26: 25 October 2026 to 27 March 2027, updated 26 October 2026) says "© 2026 The Digital Radio Mondiale Association ... All rights reserved" and gives no reuse permission, so nothing is bundled. In the app:
- DRM tab > Broadcast schedule (drm.org): "Get the schedule from drm.org" fetches the page once (about 300 KB, curl, user agent OnAir/<version> (DRM schedule)) and keeps a copy in the data folder for the next start and offline use; "Update from drm.org" refreshes it. Source, season, validity, the site's update date and the copy's date are always shown, and the credit is in THIRD_PARTY.md and the in-app Credits.
- The page's JSON (`<script id="drm-sw-data">`, 45 short wave entries now) and the India medium wave table (35 transmitters, no times) are read by `core/src/drm/drm_schedule.cpp`. On air now first (or the whole list, next start first), region filter, a button per frequency that tunes and starts the receiver.
- Find DRM: a hit on a scheduled frequency shows "scheduled now: <station (language)>".
- There was no DRM station book to wire into; the schedule list is that book.

Tests: tests/drm/test_drm_schedule.cpp on a made-up page in the site's layout (windows past midnight, two windows, Tue & Thu, Sun-Fri over the week end, "Occasionally" = maybe, frequency text with dates, medium wave rows, broken or cut pages). With DRM_SCHEDULE_PAGE set to the real page saved tonight: read, B26, 80 entries (45 short wave), 12 on air at the time of the run. The UI was compiled only, not run.

## 4. ATSC 3.0 recordings (channels 19 and 21, gofile.io/d/tnIBr10L)

Tried once more through gofile's public API (POST api.gofile.io/accounts, then contents/tnIBr10L) and the page itself: both "Recv failure: Connection reset by peer" from this network. Unreachable; no file names or sizes; nothing downloaded. The owner could ask the reporter for another host (or a direct link) and approve a download then.
