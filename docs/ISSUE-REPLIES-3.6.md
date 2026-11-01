# Issue reply drafts for 3.6 (not posted)

## #418 FM scan stops at 18%
hey Jan, thanks for the report. i found one way the scan can hang: if the radio stops handing out spectrum frames for a hop after a retune, it waited forever. now it skips that hop after 4 s and says so in the log, so it should always finish. it's on the 3.6 branch. also, can you tell me the sample rate and gain you use? with rtl-sdr 2.4 Msps works best for the scan (it looks at 2 MHz per hop). and a screenshot of the log tab after it stops would help a lot.

## #412 DMR key not decoding
hi, thanks for testing. short answer: this one is on purpose. OnAir only does the ciphers with public specs, and the Motorola/Hytera Basic/Enhanced Privacy (the RAS/key + IV part) isn't public, so it's not implemented and the key box has no place for it. DSD-FME did reverse it, we don't. what OnAir does take for DMR is AES-128/256 and ARC4 in the ETSI framing. if your radios use that, tell me the setup and ill look; if its Enhanced Privacy it wont decode here, sorry.

## #411 VDES/ASM frequencies
- A: the first two hits are the airport search. "ASM" is the IATA code of Asmara (HHAS) so its airband channels are listed first. not a bug but ill make it clearer.
- B: 162.050 is right. ASM 1 is 161.950 and ASM 2 is 162.050 (ITU-R M.2092), 162.000 is the middle between AIS 1/2... so no change.
- C: Export CSV only writes your own entries right now. exporting the search result is a good small idea, ill put it on the list.

## #410 hardware decoding on H.264
good catch, found the reason. the guard that switches to software after a few seconds was only checked for packets it fed to the GPU. an H.264 stream with no flagged keyframe (QO-100 ones with recovery point I frames) never gets fed, so it never switched and you got audio only. now it checks every packet, so it falls back to software by itself after about 6 s. on the 3.6 branch.

## #409 Tune by step in General receiver
yep, the wheel tune skipped the general receiver on purpose-ish (its click moves a marker not the radio) but it should work. now the wheel moves the listening channel by the step (and retunes the radio only when it leaves the band, same as a click). fixed on the 3.6 branch.

## #408 Tetrapol no information
the Ch column and SNR came back in 3.4.x (cells table now has Ch, Scramble and SNR). traffic info only shows when a traffic channel with voice/calls is received, a control channel only gives cells and group call announcements. what frequency and what network (country, 10 or 12.5 kHz spacing) is it? a short IQ recording would let me check. also scramble 0 usually means the signal is not locked right.
