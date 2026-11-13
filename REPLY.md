added an "STL link" mode for this, it's on the 4.1 branch (not released yet). it plays analog STLs: composite (stereo + RDS + SCA) and mono, deviation / channel width / 75 or 50 us de-emphasis are adjustable, and it tells you whether it's hearing composite, mono, or something that isn't FM at all.

digital STLs (Starlink, Intraplex etc) it only names and measures (width, level over the floor), no audio. those are proprietary QAM/OFDM frames and a lot of them are encrypted, so there's nothing public to decode against.

only tested on simulated signals so far, I don't have a real STL recording. if you can capture one (1 Msps IQ around 944-952 MHz, even 10 seconds) that'd help a lot.
