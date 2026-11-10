# Open GitHub bugs, 3.8 pass (feat38-bugs)

Branch `feat38-bugs`, based on `pub/integ-38`. Every open bug on adswill/OnAir from the brief and from the BUG / BUG-HW rows of
issues-triage.md and issues-2026-11-10.md was read with its comments (2026-11-10). Issue text and attachments were treated as data.
Before any work each one was checked against `git log --all --grep` and the docs/*-3.8.md files of integ-38 and every feat38-* branch.

Owner rule from 2026-11-10: the Mac was on battery, so nothing was compiled or run locally. The edited files were syntax-checked with
`clang++ -fsyntax-only` only. The tests run in GitHub CI (ci.yml dispatched once for this branch).

Replies below are drafts. Nothing was posted.

## Fixed on this branch

| # | Severity | Cause (proved from the code) | Fix | Regression test |
|---|---|---|---|---|
| 139, 424 | no function | The classic top bar put Radio settings, Support and the version at fixed offsets from the right edge (`disp.x - 64 - 410*gUi` ...). The language group went at "right edge minus a width", and that width left out the Touch button. On a 640 px window, or next to a wide mode selector, the buttons landed on top of the selector and of each other. ImGui gives the click to the item drawn first, so Touch, New interface, Server and the language did nothing. | `app/ui/top_row.h`: the buttons are laid out from the widths measured last frame. They go right-aligned on the selector's line when they fit, otherwise on a line of their own, wrapping at the edge. Nothing overlaps. The mode selector keeps the row's width free and turns into the drop-down sooner. | `tests/app/test_top_row.cpp`: widths 320 to 3840 px, UI scales 0.75 to 2, with and without a radio, four selector widths. No button leaves the window, overlaps another or sits on the selector. |
| 358 | no decode (audio), wrong data (frequency) | (1) A mode's `tuneOffsetHz` also moved a Hamlib rig. `noTuneOffset_` was true only for a sound card, so `radioTune()` sent the rig dial + 20 kHz for FT8, PSK, MT63, marine and the other HF modes. That is 7.094 on the 7300 for 7.074. The app side already treated Hamlib as "at the dial". (2) The Info.plist that ships, written by tools/package/make_dmg.sh, had no NSMicrophoneUsageDescription. macOS then never asks, and a HAL input gives silence. NSCameraUsageDescription and the location texts were missing too. (3) Sound card ids were the AudioDeviceID number, which changes when a USB device comes back. That would explain "the microphone is not connected anymore", but it is not proven for this report. | `DeviceInfo::tunesAtDial()` (sound card or Hamlib) in start, place, second mode and click-to-tune. Usage texts added to both plists. The input calls `requestAccessForMediaType` when macOS was never asked. Sound card ids are now `ca:<UID>`, and old numeric ids still resolve. | `tests/source/test_hamlib_dial.cpp`: the whole engine on Hamlib's dummy rig for every mode with a tuning offset, start and retune. It checks the rig's frequency, the capture centre and the dial. `tests/app/test_macos_plist.cpp`: each protected API found in the sources needs its usage key in both plists. |
| 373 | wrong data | "Send analog NFM" took its modulation from `TxRequest::kind`. In the modes that offer the box, that field is something else: FT8's message type, or the 1 the airband panel leaves there. So the fall-back could go out as AM or USB. In the general receiver the "Modulation" pick of the test signal never reached the analog voice, so switching from USB to NFM changed nothing. That is the reporter's issue 1. It also fits omegatee's "barely heard, with a buzz" when listened to as NFM. | The fall-back is NFM. The general receiver sends the audio with its picked modulation (NFM, WFM, AM, USB, LSB), and its checkbox says so. Airband and QO-100 keep `kind`, because there it is the modulation. | `tests/tx/test_tx_params.cpp`: NXDN, dPMR, TETRA and the general receiver, kind 0..3. The fall-back has a constant envelope and equal ±1 kHz sidebands. The general receiver's AM, USB and LSB picks give AM and the right sideband. |

Sibling checks: other fixed right-edge `SameLine` offsets in app/ui (learn.cpp, wizard.cpp, tour.cpp) are single items in their own
dialogs, so nothing can overlap there. For the tuning offset, every place that subtracts or adds `tuneOffsetHz` was listed with grep.
The engine sites now match the app's. KiwiSDR and SDRplay handle `tune_offset_hz` themselves and are unchanged. Hamlib TX adds the
offset only for IQ sinks (`takesIq`), so it was already right. Camera capture (TX video) on macOS had the same missing plist key,
which would end the program on first use. That is fixed by the same plist change.

Not verified on hardware: no IC-7300, no real macOS permission prompt (the TCC state on this Mac was not touched), no 640x480
window. The CI shot job renders the classic UI.

## Already fixed elsewhere (no work here)

- #357 / #430 IC-R8600 0xE600 writes: 11c4aacd7 on integ-38 (docs/ISSUES-3.8.md).
- #331 / #332 DRM xHE-AAC: the owner's c4926ec75 and 14aacfdd6 on integ-38 cut the missing frames on the #332 sample from 212 to 63 of 776.
  #331 (32 kHz mono, "refuses this stream") has no recording, so it is not known whether those commits cover it.
- #336, #338, #344, #348, #374, #376, #401, #411, #423, #454, #450, #420, #367, #339, #465: fixed in 3.3 to 3.7, and the reporter confirmed
  or the owner asked them to check. #401, #411 and #423 are closed.
- #169, #440, #468, #472 and the others in issues-2026-11-10.md: feat38-iss2.

## Not fixed: what to ask

### #319 hang after long DTMB (AVS+)
The bug report bakeair linked (mediafire, 5.9 KB, downloaded into the scratchpad and read as text) is from 3.6.0 and a radio
astronomy session at 650 MHz, not DTMB. crash.txt is an access violation in OnAir.exe+0x517776b on a std::thread, with no symbols.
From the code: a frozen waterfall plus a window that freezes on Stop means the receiver thread is blocked, and the TV player can block
it only through `Player::mu_`. No path that holds `mu_` waits on the decoder. FFmpeg's own cavs decoder refuses AVS+ (profile 0x48) at
the sequence header and then refuses every picture ("No sequence header decoded yet"), so it cannot loop on AVS+. Not reproduced, cause
unknown.

> thanks @bakeair, i looked at the zip but it's from a radio astronomy session, not dtmb, so it doesn't show the freeze. next time it hangs could you: 1) before clicking stop, make a dump (task manager, right click OnAir, create dump file) and send the size so i can say how to get it to me, 2) send onair.log from that same session, 3) say if you have the avs+ add-on and the dra add-on installed. a short recording of the service with Record in the TV tab (a minute or so) would let me replay it here

### #435 DTMB on Android
Needs the phone's log and the SpyServer rate. The owner's question from 11-09 is still open, so there is nothing new to ask. No reply.

### #470 Pi 5 llvmpipe
`GpuLdpc ... limits too small` is expected (360 invocations per work group). The renderer is llvmpipe under XWayland while Vulkan sees
V3D. That points at the X11/GLX path (XWayland without hardware GLX) rather than the GL version OnAir asks for. The owner's draft in
issues-2026-11-10.md (try ONAIR_DISPLAY=wayland, send `glxinfo -B`) is the right next step. Nothing changed.

### #192 HaLow (Morse Micro)
The IQ files are in a Google Drive folder. The folder page does not show file names or sizes without JavaScript or a login, so the size
is unknown. HaLow captures at 4-8 MHz are likely over 50 MB, so the owner needs to OK the download. No reply until someone has looked.

### #412 DMR key / RAS, #459 RSP1 no audio, #333 Tetrapol, #181 Orbcomm, #419 SDRconnect
Waiting on the owner's 11-09 questions (privacy type and recording, level meter, a 3.7 test, an IQ recording, the traffic log after
11-18). Nothing new.

## Draft replies (owner's style, not posted)

### #139
> found it, the buttons on the right of the top bar were placed from the window edge, and on a 640 wide window they landed on top of the mode picker, which then got the click. so touch, new interface, server and the language really did nothing. in the next version they move to their own line when they don't fit. until then `onair --touch` starts in the touch interface

### #424
> thanks, that was it, the buttons on the right sat on top of each other and on the mode picker, and the one below got the click. fixed for the next version, they never overlap now

### #358
> found both. the hamlib rig was tuned 20 khz above the dial for the hf digital modes (that offset is meant for sdrs, to keep the signal off the dc spike, not for a rig's own audio). and the mac app never had the microphone text macos needs before it asks, so it never asked and the usb audio codec gave silence. both fixed for the next version, it asks for the mic the first time you start the 7300's audio. the 7300's ci-v address is the hamlib default (94h), you don't need to set it

### #373
> found issue 1, sorry for saying it was the checkbox. with "send analog nfm" ticked the modulation pick of the general receiver never reached the microphone audio, it kept whatever was set before. in the next version the general receiver sends your mic with the modulation you pick, and the other modes' fall-back is always nfm. @omegatee that's probably your low audio with a buzz too, it may not have been nfm at all. can you both check when it's out?

### #319
(see above)
