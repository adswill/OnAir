# Issue reply drafts, 3.4 fixes (branch fix34-issues; nothing posted)

## #374 USRP transmit frequency incorrect
found it, thanks for the careful check with the rx channel. the general receiver tunes the radio 250 kHz above the dial (to keep the signal off the dc spike) and the analog voice path forgot to put its signal 250 kHz below the radio centre again, so it came out at dial +250 kHz. other modes already did this right. fixed now, the voice sits on the dial frequency, and there's a test that checks the spectrum. it'll be in the next build, could you check it on your usrp again?

## #373 USRP transmit mode switching + NFM CTCSS
ctcss is done: it's a pull-down with the standard tones now (67.0 to 254.1) and the level is fixed at the usual 12 percent of the deviation (about 600 Hz at 5 kHz), no slider anymore. the mode switch i couldn't reproduce yet. in the general receiver tx the usb/nfm choice is the "modulation" of the demo signal, and with "send analog nfm" ticked it's always nfm, so that's probably why it looked stuck (omegatee had the same confusion). if you still see it staying on usb with the box unticked, tell me the steps and the version and i'll chase it.

## #365 DVB-T x86 SSE2 quantise clip
great find and thanks for the patch, the x86 path really lacked the clip that the neon path had. i applied the same hunk (your pr #369 is the same idea) and added a test that fails on the old x86 code and passes now (ran it under rosetta). real recordings on x86 should lock now. about the gpu idea, let's keep that separate.

## #350 FM RDS Chinese characters
gb2312 text was already added in 3.3.0. the follow up from manhere (name never completes) is because that station scrolls its name, so the four pieces never match each other. now if the name keeps changing, after a while the latest pieces are shown as one name instead of waiting forever. gbk-only characters outside gb2312 aren't covered, if you still see "?" please send the group bytes (the rds tap or a short iq) and i'll look.

## #400 VDL2 add more channels
there's no 16 channel cap in the decoder, the list box just takes whatever you type (I made it longer anyway). added the new europe set for 2027 (136.700 to 136.975 in 25 kHz steps, 12 channels) as a "Europe 2027" button next to the channel box, and 136.675 and 136.850 are in the default european set now. the worldwide whole span list got 136.675, 136.750 and 136.950 too. those two weren't received before simply because they weren't in the default list.

## #394 VDL2 aircraft type and distance
added a type column (from the same aircraft.csv database as ads-b) and a distance column to the vdl2 aircraft table, plus both in the csv export. distance needs your receiver position set (the same one ads-b uses) and a position in the vdl2 messages, and follows the ads-b nm/km units. type needs aircraft.csv in the data folder.

## #335 DMR GPS (LRRP / LOCN)
lrrp: 3.3.0 keeps the position even when the data crc fails (it checks the ip header checksum instead), so that part should show up in the table and map now. locn is the open one: i don't have the byte layout and the dsdplus log only shows the decoded values. if you can paste the raw hex of a locn burst (dsdplus can show it with the data bytes line, like your lrrp example "Data 16 00 00 ...") i can write the decoder. the iq zips: i didn't open them, a hex line is quicker.

## #348 P25 trunk follower
the follower fix (own control channels only, 12 s dwell, no retune to the same frequency) is in 3.3.0. the "IMBE missing" in the voice strip is fixed now too: it looked only at the receiver's flag, which is empty for a moment after every retune, so it showed missing even with libmbe loaded. it asks the library now (also for nxdn and d-star). waiting on your test of 3.3.0 for the hopping part.

## #344 select and copy text
3.3.0 had copy of a whole text with hover + ctrl+c. now you can drag over a part of any text to select it (highlighted) and ctrl+c / right click copy gives just that part. works for the text panes like the acars message and details. it doesn't work in table rows that are clickable themselves (the row takes the click), those still copy whole cells only through hover.
