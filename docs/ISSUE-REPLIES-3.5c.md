# Draft replies 3.5c (not posted)

**#408 / #371 / #333 Tetrapol**
the Ch column is back in the cell table (country, system, cell, LA, BN, Ch, scramble). i kept SNR too, it's in the summary line. also fixed a frequency control thing so small offsets keep the call signalling. can you try the 3.5 beta?

**#405 Web-888**
the web-888 runs the kiwisdr server on 8073, so add it as type KiwiSDR with its address, it should just work for the 12 kHz iq / hf modes. i don't have one so can you tell me if it connects? added a note to the network radios doc.

**#404 / #407 / #403 / #401**
these are fixed on the 3.5 branch (fgb false accepts, elt-dt protocol 9, automation distance, the 3.2 msps rate for the smart v5). will be in the next beta, thanks for the detailed reports.

**#398 Pluto dual**
dual rx/tx on the clone ad9361 isn't in yet, onair opens one rx channel. i don't have the board so i'd need someone to test, would you try a build if i add a second channel option?

**#397 SGB recordings**
thank you, that's exactly what i was missing. can you drop the two sigmf files (or a link) here? i'll add them as test data and fix the docs line.

**#393 stuck on 1090**
good catch zoeloe, 3.4.0 fixed the carry itself but a frequency saved by 3.3.0 in your settings kept winning over the mode default. now a saved 1000-2100 mhz value is ignored for modes whose own default is far from it, with a test. iptv at 100 mhz is just its default (it's not an rf frequency).

**#390 hf fax**
the picture no longer restarts at 1500 lines, the limit is 8000 now. the missing stop tone and the "realign lines" editor i need a recording for, can you send a short iq/audio of a jmh chart?

**#389 carrier info / filter**
added a "Show" box over the dmr calls table: all / voice only / group voice only / hide control / data only. saving the decoded messages with the carrier is in the 3.5 beta (save decoded messages, one file per day per mode).

**#384 several modes at once**
this is what multi-vfo does, you place a signal on the spectrum and give it its own mode/bandwidth (docs/MULTI-VFO.md). a full cubicsdr style many-demods view is on my list, i'll look at replacing the second mode with it.

**#375 chinese**
the whole ui has a chinese translation already (zh.txt) but i'm not a native speaker, if you or others spot bad wording send the lines and i'll fix them.

**#372 threads**
you can set the number of decoding threads now: put threads=4 in the settings file (or DECT2_THREADS=4). it's applied at the next start. a box in the settings window is coming.

**#370 codecs**
the guide is docs/CODECS.md and the ? button in the app. for yaesu c4fm the codec id is ysf, so the file is onair-codec-ysf.dll next to the exe in the codecs folder. tell me what step fails.

**#367 pluto low rate**
which pluto firmware (and is it a pluto+ or a usb network adapter)? the lowest rate is 2.048 msps and needs the "low-rate FIR" option on in the radio settings. is that what you were missing?

**#358 ic-7300**
thanks for the details. the 20 khz could be the rig's own shift or the data mode, can you check the rig display in usb-d? for hamlib the address is picked by the model (94h for the 7300). for audio on macos, which app asks for the mic permission when you run onair from the dmg? can you send the debug log?

**#357 r8600 firmware**
loosened it: the structure check now accepts the 512 bytes of scratch ram the vendor package also loads, the exact hash check stays. try again, if it still refuses it shows the sha-256, send that.

**#344 text select**
that's in since 3.4, you can select and copy text in the tables and boxes. does it do what you need?

**#341 / #339 dvb**
the x86 viterbi clip (peter's find) is in 3.4.0. can you try 3.4.0 on the same muxes?

**#340 / #337 v4 lite**
the driver we ship is the rtl-sdr-blog one from march (the commit with the v4l support), so it should be right. can you send the debug log of the radio line when you start it, and tell me if it finds the rtlsdr.dll you put in the folder instead of ours?

**#338 keep frequency**
fixed in 3.4.0, the carry is only between the satellite dish modes now.

**#336 linux font**
glad it works, closing this one if you're ok.

**#331 / #332 drm audio**
the log says libfdk-aac refuses that xhe-aac stream. can you send a short iq recording of it so i can check the usac config?

**#328 same serial rtl-sdr**
sorry it's still broken. each dongle now gets its own key from the list position, but i can't test with two dongles. can you start onair with the debug log on and send the part where the second one fails?

**#249 dab transmitter list**
thanks for the recording, i'll look at what your ensemble sends.

**#255 rwm**
right, rwm only sends ticks and no code. i'll look at showing the system clock calibrated to the ticks.
