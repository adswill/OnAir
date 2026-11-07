# Bug hunt 3.7-B (voice / ham / aviation / sensors / sources)

## Fixed
1. DMR: the embedded link control (5-bit checksum only) set the privacy ("encrypted") flag, or swapped a call's IDs, from a single decode. Now privacy and an ID change need two embedded LCs in a row that agree (core/src/dmr/dmr_link.cpp). Regression: tests/dmr/test_dmr_impair.cpp clear-call SNR ladder 4-11 dB, base and direct (0 flagged encrypted). test_dmr_plugin (encrypted still skipped) and test_dmr_air pass. Note: the ladder passed before the fix too (no natural false pass reproduced in 20 s runs); the guard is by construction, not shown to fail without it.

## Reviewed, no change
- P25 HDU/LDU1/LDU2/TSBK flags are RS/CRC protected. D-STAR, dPMR, Yaesu Fusion carry no cipher flag.

## Not covered
Only DMR was swept; FT8/WSPR/APRS/pagers/ADS-B/ACARS/AIS/sensors/sources not reviewed; no CI sweep dispatched.
