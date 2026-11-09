# Draft replies for 3.7 (NOT posted; the owner posts them, only after 3.7 is released)

Written in the owner's casual style from /private/tmp/smart37/issue-replies.md and pr-replies.md. Wording in "ships in 3.7" lines holds only if the release goes out as planned.

How to read this file:
- **Fix verified in 3.7 work**: the fix was made in this release cycle and read in code (not run on the reporter's hardware).
- **Unverified, check before replying**: the triage marked these 97 issues as fixed from the issue text and a cited commit alone, without reading the code. Check the cited commit and, where it matters, the current behaviour, before you post anything.
- Not included: issues that are not fixed (questions, hardware reports waiting for the reporter, features). See the end.

# Part 1: pull requests merged in 3.7

## #461 (DME-3), fix/modebuilder-fsk-acquisition, closes #460

thanks a lot, really nice one. the quarter deviation guard is small and the tests with the real one tone controls are exactly what i wanted to see. merged into the 3.7 branch, ships in 3.7


## #447 (bellx2), rtlsdr mac read thread

thank you, great measurements, 0.4 down to 0.03 is a big difference and keeping read_sync is the right call after #14. merged for 3.7. it might also help the dropped frames in #210, we will ask there once its out


## #445 (peterdewitte), windows camera and mic

thanks peter, finding --disable-avdevice was a great catch, explains all those no camera reports. mic by name and the privacy hint are really nice too. merged for 3.7. the v4l2 follow up for the linux packages is welcome if you feel like it


## #467 (peterdewitte), LoRa APRS transmit

thanks peter, nice one, real lora aprs packets instead of meshtastic and the settings under advanced is how it should be. merged for 3.7, i only gave the comment box its own id because gui_rules complained. if you get a chance to try it against a real tracker or igate let me know how it goes


## #446 (peterdewitte), videogen hardware encoders

thanks, the shared setup for the trial and the start is a really clean fix, and the (!) tooltip is great. merged for 3.7. two things on my side, it clashed a bit with your easy mode pr so i joined them in the merge, and h264_videotoolbox on mac failed your new test with b frames (muxer said invalid argument) so i turned those off for it like you did for mf. about x264 in the packages, lets leave that out, we dont bundle those, vendor encoders first like you said


## #444 (peterdewitte), videogen easy mode and preview

thanks peter, easy mode is exactly what this window needed, and thanks for the second start crash fix too. merged for 3.7 together with #446, the encode bit needed joining by hand. small thing for later, the preview still opens the video encoder even though it never sends it a picture, could be skipped so a hardware encoder isnt held for nothing


## #437 (BI8SYN), chinese fonts

thanks a lot for cutting it down so far, 6.8 MiB is great and the language isolation tests are really good. merged for 3.7, squashed like you asked so the big fonts stay out of the history, you are the author on the commit. i also added python to the windows full ci and the linux docker images since the build needs it now


# Part 2: issues fixed in the 3.7 work

## #450 HackRF + RTL-SDR unstable together
found two things in the code. your "Enabled direct sampling... PLL not locked... Disabled" lines: the blog librtlsdr retunes to its last frequency when the sample rate is set, on a fresh open that's 0 Hz, so the R820T went into direct sampling for a moment (the radio setting Off can't stop that, the library does it itself). if the PLL then fails, it stays in direct sampling at 0 Hz. fixed by tuning before setting the rate. second, the auto gain shared one state between RX 1 and RX 2, so with two receivers they kept reconfiguring each other, that's the ADC swinging on both. fixed, per receiver now. no hackrf + rtl here to test both together, so please try the next build. was Auto gain on for either receiver? and does the rtl log still show those lines? why the PLL fails only with both radios i can't say yet

## #420 Pluto won't start at 2 MHz in some modes
found it: a sample rate you chose went straight to the AD9361, and it refuses anything under 2.083 Msps without its FIR. DAB worked because its default rate is lifted to 2.1 automatically. now if the radio refuses a low rate it runs at 2.1 Msps and the receiver resamples, with a note in the status. tested against a simulated pluto only, can you try it with your clone? which firmware and did the status line show that note? also tell me if 6 -> 2 MHz still behaves the same

## #367 low sample rate on Pluto over network
the list started at 2.048 Msps for every pluto, network or usb. it now goes down to about 0.5 Msps (under 2.1 the radio still runs 2.1 and the software does the rest). not tested on a real pluto, if your friend can try the next build and tell me the firmware (fw_version in the log) that helps

## #340 / #337 RTL-SDR Blog V4 Lite on Windows
you were right, sorry. the windows package was built from an old library folder, its librtlsdr.dll is the 1.3.6 one with no V4 Lite in it, the build only rebuilds what's missing so 1.4.0 never got in. the build now rebuilds it and refuses to package an old one. no V4L here, so can you check the next windows build without your Release.zip? is the frequency right and is there signal?

## #439, #454, #460, #210 (from this cycle's commits)

## #439 ATV (M / NTSC): video and audio out of sync
fixed for 3.7, a video file now plays against the signal clock instead of at decoder speed (41dd5444f). please try 3.7 and tell me if the drift is gone

## #454 TX: demo mux does not fit the channel rate
the DVB-T2, DTMB and DVB-C muxes are padded to the channel's real bit rate now (879c0fd60). try it on 3.7

## #460 DME-3 recording does not decode
fixed by #461 from DME-3, in 3.7, can you try it on your recording

## #210 DAB+ dropped frames on RTL-SDR v3 on macOS
#447 from bellx2 should help the dropped frames on the rtl-sdr on mac, cant promise its all of it, can you try 3.7 with dab+ and tell me

# Part 3: UNVERIFIED, check before replying (97 issues)

## [unverified, check before replying] #66 HFDL ne fonctionne pas
hfdl decodes for you since 3.0.0. the ads-b switch stuck at 1090 was the carry-over-frequency bug, fixed in the 3.7 line. please retest on the next build and tell me

## [unverified, check before replying] #78 Program closes when AirSpy Discovery is stopped
reporter says it's gone since 0.2.7 and ran fine overnight on 3.3.0, so closing it from my side is fine

## [unverified, check before replying] #79 Meteor M2-3 LRPT fail to decode.
rs lock over dropouts is fixed in 8f47b2cd6 (rows placed by onboard time). if the picture still breaks up on a good pass send the iq and i'll look

## [unverified, check before replying] #84 SoapySDR over lan
glad it works. the hint in 'Add network radio' now says what to check (server, firewall, prot=tcp)

## [unverified, check before replying] #88 DTMB video and audio always frozen or jitter even C/N is very high and
ldpc is much faster now (gpu on macs) and the packet pacing is tested for every dtmb mode. can you try the 698 MHz capture with the newest version and tell me if it still freezes?

## [unverified, check before replying] #96 Bug Report: DVB-T/T2 8 MHz Signals Not Detected or Locked with PlutoSD
set Link to Ethernet and use 10 Msps for 8 MHz channels, it should lock now. let me know

## [unverified, check before replying] #103 Weak WEFAX signals
narrower filter before the discriminator went in 3.0.0. tell me how it compares on your rsp1a, if it's still weak a short recording would help

## [unverified, check before replying] #104 'assert()' in '_DEBUG'-mode
fixed in e011c16cf, debug builds no longer assert there

## [unverified, check before replying] #121 [Feature Request]Proposals for adding DRA+ decoding support for CDR br
the audio cutting out was fixed (it recovers after a sync loss now). dra audio is a separate thing, still looking at that

## [unverified, check before replying] #139 Feature: Alternate Touch-Friendly Interface?
small screens start in the touch interface now. the min button size on very small panels is still on my list, send the screen size of your device

## [unverified, check before replying] #148 Adding channel to airband does not save
adding, editing and deleting airband channels works and they're saved now. squelch control is fixed too, try the newest build

## [unverified, check before replying] #164 LTE+NR Scanning
mfbi list and bands above 64 come from sib1 now, and list filters are in. check band 66/71/14/13/4 on the newest build

## [unverified, check before replying] #166 Crash when clicking on services in video decoders.
fixed in 3.3.0 as you confirmed

## [unverified, check before replying] #167 Enhanced WiFi HaLow
4, 8 and 16 mhz channels are in. try the newest build please

## [unverified, check before replying] #178 Bug: Incorrect display. Please change the label for DVB-T2 mux to DTMB
label is right since 3.5. the remaining english bits in the chinese ui (Player, Guide, Recordings, Teletext) are tracked in #375

## [unverified, check before replying] #183 Feature Request: Add fallback mirror/proxy for update check and asset 
there's an optional update mirror now (Settings, off by default), checksum still verified against github

## [unverified, check before replying] #189 Globalstar frequency labeling, GNSS sync drops, Iridium features, and 
the globalstar text says uplink now. gnss drop reasons are logged. am and variants are in the general receiver. the rest of your list is noted

## [unverified, check before replying] #192 Morse Micro HaLow Networks
scan reaches the outer channels now. if morse micro still isn't recognised send a short recording of it

## [unverified, check before replying] #201 Android app crashing upon starting SDR stream
rtl_tcp fix is in. please try the latest apk and send the log if it still freezes on start

## [unverified, check before replying] #210 OnAir 0.2.7 (and 0.2.6) crash when using DAB+ on RTL-SDR v3 dongle
the crash is fixed. the dropped frames on rtl-sdr v3 on macs are the usb fifo overflow, there's a fix waiting (pr 447)

## [unverified, check before replying] #213 Feature requests: FM audio options, C-QUAM AM stereo, VHF-DSC, and sat
fm options, c-quam and clear tetra voice are all in. vhf dsc has its own mode now, tell me if something is missing

## [unverified, check before replying] #215 Automatic search and configuration bricks unattended devices
the radio list never scans the network unless you switch it on, as you confirmed

## [unverified, check before replying] #218 Mesh (and other modes): signal time or the computer's clock for packet
setting is in Settings: computer clock or signal time. reporter's test shows it works

## [unverified, check before replying] #219 SDRPlay RSP1 weird tuning frequency on click
clicking the spectrum tunes the clicked signal now. try 3.3.0+ with your rsp1

## [unverified, check before replying] #223 [Feature request] Wider bandwidth for the Cospas beacon decoder and a 
both of your recordings decode, no false decodes. wider than 32 khz is a separate request

## [unverified, check before replying] #225 [Feature Request]Add Unicode support to the Hellschreiber modulator
accented latin, cyrillic and greek are in the modulator. cjk fonts are still to do

## [unverified, check before replying] #226 Onair software cannot run
3.0.0 builds for plain x86-64 and says so in plain words if the cpu is too old. try the newest build on the i5

## [unverified, check before replying] #228 MacOS SoapyRemote hangs OnAir till Force Quit
open gives up after 20 s instead of hanging. try the newest build

## [unverified, check before replying] #229 Decoder request LORa Satellite
lora sat presets are in; more frequencies are on the list

## [unverified, check before replying] #235 [Feature Request] Aircraft information on full screen map
panel on the full-screen map works; i'll make the empty bar vanish when the aircraft is gone

## [unverified, check before replying] #236 Feature request: Better usability for ultra-wide-band SDR's
whole span for vdl2 and hfdl is in. acars across the whole span is still to do

## [unverified, check before replying] #237 Feature request: Control waterfall speed
works now, thanks

## [unverified, check before replying] #238 Bug: Changing frequency after zooming resets zoom level
zoom stays when you change frequency now

## [unverified, check before replying] #239 [Feature Request] Analogue TV
snow option is in. i'll check the freeze on double click full screen

## [unverified, check before replying] #243 Feature request: Waterfall better resolution
finer fft and aligned waterfall are in, tell me if narrow signals look sharper now

## [unverified, check before replying] #247 DRM broadcast, xhe-aac cannot be decoded
xhe-aac needs your own libfdk-aac in the codecs folder. the stream message now says exactly what the decoder refused

## [unverified, check before replying] #248 Docs: Network radios: rtl_tcp, SpyServer, SoapyRemote and more
tci works with LOCALHOST as the server, thanks for testing

## [unverified, check before replying] #249 NO DECODE OF TRANSMITTER LIST IN DAB
dab map keeps every transmitter heard over a band scan and the fullscreen map is full height. you can also import a transmitter list (csv format in the docs). try it with your 178.352 recording

## [unverified, check before replying] #252 Enable Dmr audio
dmr voice needs an AMBE+2 codec plug-in you install yourself, see docs/CODECS.md and the ? help in the DMR tab

## [unverified, check before replying] #254 DTMB radio broadcasting is very choppy
pacing fixes went in 3.0.0 and 3.2.0. try the newest build and tell me your cpu, send a short iq if it's still choppy

## [unverified, check before replying] #257 KIWI SDR "radio stopped sending samples..."
kiwi stalls fixed (frame wait, restarted sequence, handshake). try the newest build and send the log if it still says radio stopped

## [unverified, check before replying] #258 Feature Request from V0.29
10 khz tetrapol spacing is in and the osm 403 is fixed with proper identification

## [unverified, check before replying] #264 Feature Request: Noise Blanker
glad it helps

## [unverified, check before replying] #266 About how to activate xHE-AAC
drm offset fixed. xhe-aac needs libfdk-aac in the codecs folder, the message now says which step fails

## [unverified, check before replying] #269 Feature Request: frequency detection and capcode detection functions f
capcode watch list and whole span hint are in. automatic detection of unknown parameters is still to do

## [unverified, check before replying] #270 Waterfall misalignment
fine on 3.4.0 as you said

## [unverified, check before replying] #271 Ideas regarding adding NFC decoding functionality
nfc mode exists and decodes captures like yours

## [unverified, check before replying] #274 Requesting variable bandwidth
you can set the bandwidth up to 2 mhz for those now

## [unverified, check before replying] #275 BUG:CDR Decode Failed
cdr locking was fixed in 3.2.0, try your recording with the newest build

## [unverified, check before replying] #277 BUG:Analog TV transmission is slow
faster since the test card is no longer redrawn. tell me how it is now

## [unverified, check before replying] #279 some codecs cannot be found when the app is installed in a non default
codec search covers non default install folders now (and the appdata folder)

## [unverified, check before replying] #281 Additional functionality for Iridium
iridium acars, packet dump and pager views are in; voice plays with a codec you install

## [unverified, check before replying] #283 OnAir silently exiting on lauch under Debian Trixie on Raspberry CM5
works since 3.0.0 as you confirmed

## [unverified, check before replying] #284 No waterfall under Debian Trixie on raspberry CM5
works since 3.0.0 as you confirmed

## [unverified, check before replying] #291 Issue with 'setvbuf()'
fixed in a14e42e2f

## [unverified, check before replying] #296 waterfall
old style waterfall is back as an option, thanks

## [unverified, check before replying] #297 Some bugs about v3.0
status bar text and the other items are fixed, retest on the newest build

## [unverified, check before replying] #299 Some confusions
drm text is clean now; tianqi frequency is added. libfdk goes in the codecs folder

## [unverified, check before replying] #300 dmr sms
fixed, little endian text is read now

## [unverified, check before replying] #301 Two Bugs Found
both fixed in 3.2.0. the general receiver centres differently from ft8 on purpose; tell me if you want it to match

## [unverified, check before replying] #303 Request. in the VDES mode.
vdes positions show on the map now

## [unverified, check before replying] #308 PSK demodulator
manual bandwidth is in

## [unverified, check before replying] #309 VDL2 log export
vdl2 session log with utc time and csv/json export is in

## [unverified, check before replying] #310 analog tv
analogue tv is in

## [unverified, check before replying] #313 ISDB-T: one-seg service does not play when the PAT is missing (layer B
one-seg plays without the pat now. layer b is still open, a stronger capture of it would help

## [unverified, check before replying] #316 PlutoSky R2 – CMA allocation errors at 10 MS/s
network pluto uses the native driver now. libiio is the one we ship, version noted in THIRD_PARTY.md

## [unverified, check before replying] #320 TX panel colours ar very difficult to read
tx text readable on the light skin now

## [unverified, check before replying] #321 Incorrect tuner rate for remote receivers
spyserver range is correct now

## [unverified, check before replying] #328 Bug: multiple receivers of the same type seemingly not possible
fixed, two dongles with the same serial work

## [unverified, check before replying] #335 DMR GPS position reports (LRRP and LOCN) apparently not decoded: nothi
lrrp positions with bad packet crc but good ip checksum are kept now. try the newest build

## [unverified, check before replying] #336 Language set to 'same as system' the UI fonts unreadable in linux of u
fixed as you confirmed

## [unverified, check before replying] #337 RTL-SDR Blog V4L: no reception at all with OnAir-bundled driver; with 
v4 lite is supported in 3.3.0, but old installs keep the old library, see #340

## [unverified, check before replying] #338 Request: Keep selected frequency when possible
keeping the frequency now only applies between satellite modes and the mode default wins elsewhere (9f01658b2)

## [unverified, check before replying] #344 [Feature request] being able to select text, copy to clipboard
drag select and ctrl+c work in 3.4.0

## [unverified, check before replying] #348 P25 trunk following (3.2.0, Windows): follower leaves the control chan
control channel follower fixed in 3.3.0/3.4.0

## [unverified, check before replying] #350 FM RDS: Chinese characters in PS/RadioText are displayed as "?" — non-
chinese rds and scrolling names are handled now

## [unverified, check before replying] #357 IC-R8600
firmware import accepts those writes now. the python loader workaround is no longer needed

## [unverified, check before replying] #365 DVB-T x86: SSE2 quantise() misses the ±limit clip for the 8-bit Viterb
merged, thanks for the patch

## [unverified, check before replying] #371 Tetrapol Ch
ch column is back in 3.5.0

## [unverified, check before replying] #372 he program can be customized to set the number of threads and the amou
threads=4 in the settings file or DECT2_THREADS=4 works from 3.5.0. a box in Settings is next

## [unverified, check before replying] #373 Bug Report: USRP Transmit Mode Switching and NFM CTCSS Tone Selection
ctcss pull-down is in; tx mode changes apply every time now

## [unverified, check before replying] #374 Bug Report: Incorrect USRP Transmit Frequency
the 250 khz offset is fixed in 3.4.0, good catch

## [unverified, check before replying] #376 issue with mp3 lame dll for recording every update
fixed in 3.5.0, put it in the codecs folder

## [unverified, check before replying] #383 Feature Request: Data Folder Selection
fixed in the next build: the data folder buttons had clashing ids so clicks did nothing. please retest

## [unverified, check before replying] #386 Mesh (LoRa) on RTL-SDR: sample jumps break frames at 2.048 Msps (rtl_t
sample jump recovery is in 3.5.0, thanks for the numbers

## [unverified, check before replying] #389 [Feature request] Log Carrier Information / Filter by Type
calls filter by type is in. i'll add the carrier column to the saved results

## [unverified, check before replying] #390 Feature Request: for existing Weather HF Fax
images go up to 8000 lines now. the click-to-realign editor is a separate request

## [unverified, check before replying] #394 VDL2 type d'avions et distance
type and distance columns are in vdl2 now

## [unverified, check before replying] #400 VDL2 add more channels
europe 2027 channels are added and there is no 16 channel cap

## [unverified, check before replying] #401 [Bug] Incorrect data for NESDR SMArt v5
sample rate limits fixed in 3.5.0, thanks

## [unverified, check before replying] #404 False FGB decodes on live 406 MHz traffic and cross-triggering from re
isolated bogus fgb decodes are rejected now, retest and say if any still slip through

## [unverified, check before replying] #405 [Feature] Add support for Web-888 receivers
add it as type KiwiSDR with its address. tell me if it connects, i don't own one

## [unverified, check before replying] #407 ELT-DT location protocol 9 is not decoded: unknown type, no position, 
protocol 9 decodes as ELT-DT now with a stable id

## [unverified, check before replying] #408 Tetrapol no information
ch column is back

## [unverified, check before replying] #410 Feature Request: Automatic deactivate hardware decoding for DVB-S2 / H
hardware decode now falls back to software for h.264, try QO-100 again

## [unverified, check before replying] #418 Broadcast radio, FM stereo and RDS scan issue
fm scan no longer stalls at a hop. try the newest build and raise the gain if it's still silent

## [unverified, check before replying] #456 Discord links unusable
new invite link is in (9040dabdc), old one was expired

# Part 4: PRs not merged (replies if you want to close them)

## #448 (peterdewitte), ffmpeg shared libraries (draft)

thanks peter, lots of work in this. going with what you said in your comment, delay loading adds stability risk, so i wont merge this one. also 3.7 already has a simple external ffmpeg folder setting now so it would clash with that. ok to close it? if you want to do something smaller on top of that setting im happy to look


## #414 (BrunoKlu), Aero C voice

thanks again for this one, the parts we could use went in for 3.6 with you as co author (a161b3310), the rest would duplicate our own aero c code so nothing more to take from here. ok to close it

# Not included (not fixed, or only a question / hardware report)

#134, #163, #169, #181, #184, #211, #251, #255, #256, #317, #333, #339, #358, #370, #412, #419, #435, #440, #457, #459, #468, #319

#319 (hang after long DTMB reception) has no fix; the earlier draft asked the reporter for onair-bug-report.zip and the AVS+ add-on version.
