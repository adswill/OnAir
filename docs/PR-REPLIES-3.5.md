# PR reply drafts for 3.5 (not posted)

## #406 (peterdewitte): make the tests on main pass again, print speed figures

Hey Peter, thanks a lot for this one, big cleanup. I pulled it into our 3.5 integration branch with you as co-author. Three files conflicted (mesh_rx.cpp header comment, test_netradio.cpp, test_sarsat_rx.cpp): the Red Pitaya 0..40 dB and the sarsat file path fixes had already landed on our side in nearly the same form, so I kept ours there and took everything else of yours as is, including the web_hub procMu_ hang fix, shorter mesh_rates with --full by hand, and the CPUCAL lines. Will see the CI numbers once the run finishes, thanks for setting up the speed figures, that will help a lot calibrating the limits.

## #366 (Curium-3391): fix(fm) stabilize live status readout layout

Hey, thanks for this, the jittering FM status row annoyed me too. Looks good: grouping each label+value and reserving widths is a clean way to do it and it stays UI-only. I cherry-picked it as is (your authorship kept) into 3.5 and it builds. Not tested on a real RTL-SDR V4 on my side yet, if you can try a build with it and tell me if the row stays still that would be great.
