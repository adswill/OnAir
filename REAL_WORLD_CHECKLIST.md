# What can go wrong with a real signal

Go through this list whenever a receiver is written or changed. For each item, either the code handles it (say where) or there is a reason it cannot happen for this standard. A test with OnAir's own generator only proves that the receiver agrees with the generator. Both can share the same wrong assumption.

## The radio
- **Tuning error.** Cheap radios are off by 1 to 50 ppm: at 600 MHz that is up to 30 kHz, and at 1.5 GHz up to 75 kHz. The error drifts as the radio warms up. In OFDM it is usually many whole carriers plus a fraction. Removing the whole-carrier part by re-indexing does not undo its phase rotation from one symbol to the next.
- **Channel not centred.** The user tunes beside the channel, or records with gqrx or SDR# at an offset. The signal can sit anywhere in the sample band, as long as it fits.
- **Sample clock off by tens of ppm.** The symbol timing then drifts, and over a long frame the error adds up.
- **Unusual sample rates.** For example 2.048, 2.4, 2.56, 8, 10 or 20 Msps, a rate that is not a whole multiple, or the wrong rate typed in by the user.
- **Hardware artefacts.** A DC spike at the centre, IQ imbalance, a swapped I/Q or mirrored spectrum (some radios, some file formats), and 8-bit quantisation.
- **Signal level.** Too weak, clipped, or with the AGC moving during reception. A strong neighbouring channel and its images.
- **Lost samples.** USB drops, a sample stream that stalls and then bursts, and a file that starts or ends in the middle of a frame.

## The air
- **Multipath.** Echoes longer than the guard interval, a stronger echo than the direct signal (SFN), echoes that arrive before the main path, and fading or Doppler (cars, aircraft, satellites).
- **Interference.** Narrowband carriers, impulse noise, and analogue neighbours.
- **Several transmitters on one frequency.** SFN, TII, two sites with the same ID.

## The transmitter
- **Every option the spec allows**, not only the common ones: every FFT size, guard interval, pilot pattern, modulation, code rate, extended carriers, MISO, FEF parts, several PLPs or subchannels, and time interleaving depth.
- **Older and newer spec versions.** A field that is reserved in one version may be used in another, so check the version field before trusting it. Reserved bits can be set, for example for bias balancing.
- **Counters and indices that wrap** at a value the signalling gives, not at 256: frame index, super-frame, CC, sequence numbers. A PLP can skip frames (FRAME_INTERVAL).
- **Settings that change during reception:** a reconfiguration, a new service list, a changed modulation.
- **Signalling that is legal but unusual:** empty tables, very long tables split over sections, text in unusual character sets, and zero services.
- **Real transmitters bending the rules:** wrong CRCs in optional fields, stuffing, null packets, and padding.

## The program
- **Starting at a random point:** in the middle of a frame or a table, or with the decoder in a stale state from the previous channel or mode.
- **Retuning, changing mode, and pausing and resuming** while data is in flight.
- **Speed:** the decoder falls behind (slow CPU, the GPU falling back to the CPU) and must catch up or drop data cleanly, not fall apart.
- **Bad input:** NaN or infinite samples, zero-length blocks, truncated or corrupted files, and garbage that happens to look like a sync word.
