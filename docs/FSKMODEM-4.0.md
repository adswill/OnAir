# FSK modems: Bell 103, V.21, V.23, Bell 202 (4.0, family 780)

Reads telephone-style asynchronous FSK as audio tones in a single-sideband channel (the Canadian time station CHU sends Bell 103 answer tones, 300 baud, on its upper sideband). Standards: Bell 103 originate (1070 / 1270 Hz) and answer (2025 / 2225 Hz), ITU-T V.21 channels 1 (980 / 1180 Hz) and 2 (1650 / 1850 Hz) at 300 baud, V.23 mode 2 (1300 / 2100 Hz) and Bell 202 (1200 / 2200 Hz) at 1200 baud.
Shares the x40 receiver (`core/src/x40/x40_fskmodem.cpp`, test `tests/x40/test_x40_fskmodem.cpp`, generator `tests/x40/x40_gen4.h`). Variant 43, family 780, active standard 781, engine standard 782.

How it works: bursts of tone energy are found in 32 ms frames (8 times the noise floor for 0.1 s). A burst of 0.4 s or more is analysed: its spectrum must hold 9 dB over the noise in one sideband, 60 % of it near the two tones of a standard. Each standard is tried with a dial error from -40 to +40 Hz (10 Hz steps), then refined to 2 Hz:
matched correlators per tone over one bit give mark minus space, each start bit is a crossing, the eight data bits and the first stop bit are read at their centres (a clock error of a few percent costs nothing). A standard counts when it gives at least four bytes, at most 15 % framing errors and decisive bits; the one capturing the most tone energy wins.
The row shows the standard, the sideband, the tones, the bytes in hex and ASCII and the framing errors. It does not know what the bytes mean (the time code of CHU has its own format).

Tested (simulated signals, 48 ksps): all six standards in both sidebands with a dial error of 0 / +28 / -30 Hz, a clock error of 0 / +1 % / -1 %, 1 or 2 stop bits, idle gaps between bytes, 25 dB tone to noise in 3 kHz: 36 of 36 bursts read with all 24 bytes right and named right.
Noise sweep (Bell 103 answer and Bell 202, 3 bursts each, tone power to noise in 3 kHz): 30, 20 and 14 dB: all bytes right; 10 dB: 2 of 3 bursts, bytes right; 6 and 3 dB: no message (and never a wrong one). Noise alone, one tone, two tones that are not a standard, and a 600 baud burst on the V.21 tones: no message.
**No recording of these modems was available**; CHU's own byte format was not tested.
