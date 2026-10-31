# Issue reply drafts, 3.5 fixes (branch fix35-issues; nothing posted)

## #407 ELT-DT location protocol 9 not decoded
thanks, this was a proper find and the field layout from your comparison with dec406 made it easy to check. protocol 9 is now recognised as ELT-DT: id type (bits 41-42, aircraft address when it's 0, "identification not available" otherwise), the PDF-1 position at 0.5 degrees, and with a good second check the activation, altitude, location age and the minute/4-second offsets, so the composite position comes out (43.5 N 1.5 E with +3m36s / -1m08s gives 43.56000 N 1.48111 E in the test). the 15 hex id now ignores the position bits, so one transmitter stays one beacon. one honest caveat: i only had your numbers, not the raw bits of a burst, so the test is built from your example by hand. if you can send a short sigmf of the france test burst i'll add it as a real regression.

## #404 false FGB decodes on 406 MHz
agreed that repair-3 on 82 bits is weak on its own (your 4.4 percent number is right). tightened the acceptance: a message that needed 3 repairs now needs a perfect 24/24 sync word, the country code has to be a real MID, and a long message with 2 or more repairs needs its second check intact. that should remove the isolated one-offs like the A618ED... one. i have NOT yet fed your sgb recordings from #397 through it (couldn't test with the real files here), so the sgb cross-trigger is not claimed fixed. repeat-before-listing and marking marginal ones as unverified is a bigger change, left open. a clean replay of your 3 s sigmf would help.

## #403 automation notification missing distance_km
you were right, the recipe uses {{distance_km}} but only the sonde event had that field filled. now any event with lat and lon gets distance_km from the receiver position (when it's set in the ads-b window), so the notification shows it. added a test with a rule like the recipe.

## #401 NESDR SMArt v5 rate range
the list stopped at 2.56 Msps on purpose because of dropped samples, but you can choose up to 3.2 Msps now for every rtl-sdr (the dongle is sold for that). the automatic rate still stays at or below 2.56 so nothing changes unless you pick a higher one. the frequency side (-100 kHz) needs the direct sampling input of your unit, which i can't verify, so that stays as is. if you tell me which branch the nesdr hf input uses (i or q) i can add it.

## #386 mesh on rtl-sdr sample jumps
thanks for the really detailed measurements. went with your proposal 1b: for an rtl-sdr (usb or rtl_tcp) a lora channel up to 500 kHz now runs at 1.024 Msps; other radios keep 2 Msps. it means only the centre channel is watched on an rtl-sdr. proposal 2 (recovering frames with a jump through the hamming parity) is not done, it needs your frames to test properly, maybe later.

## #383 data folder selection
added a "Data folder" box in the sidebar settings (browse, use this folder, default). it takes effect after a restart and the old files are not moved, so copy the OnAir folder over yourself first if you want to keep your maps. the choice is stored in a small datadir.txt in the normal folder. DECT2_DATA_DIR still wins if set.

## #376 libmp3lame.dll removed on update
not intended, and it's the updater replacing the install folder. onair now also looks for libmp3lame in the codecs folder (%LOCALAPPDATA%\OnAir\codecs on windows, the same folder the voice codecs use), which updates don't touch, and tries that before the program folder. put the dll there once and it stays. (the old note about LocalAppData not working was because that folder wasn't searched for mp3 before.)

## #367 pluto over network, low sample rate
not fixed, i don't have a way to reproduce it. the listing starts at 2.048 Msps (the ad9361 itself starts at about 2.1, the 2.048 comes from the optional low-rate fir). if you mean the sample rate list, that is the reason. which firmware does the friend's pluto run (and is it a pluto+ or a usb-network adapter)? with that i can look at lowering the minimum.

## #398 pluto clone (ad9361) dual rx/tx
not done. the second channel (voltage1) is only used for tx on firmware that has it; receiving on both channels at once isn't supported yet and i can't test it without a board with two rx, so i'd rather not guess. noted for a later version, tezuka is the right firmware for it.
