# Bug hunt 3.7-D (ADS-B, ACARS, VDL2, HFDL, AIS, EPIRB, sensors, ISM, voice trunking, network sources, drivers)

## Fixed
1. SDR++ server and SpyServer clients: a garbled scale field (NaN) or float samples that were NaN / infinity went straight into the IQ ring and stayed in every filter and AGC until Stop (the TCI source already guarded this). Now replaced by silence (`scrubNonFinite`, core/src/source/native_common.h). Regression: tests/net/test_sdrpp.cpp `testGarbledScale`, which fails (`nonFinite == 0`) with the call disabled and passes with it.

## Added test, no bug found
- tests/source/test_rtltcp_chunks.cpp: rtl_tcp stream in random 1..4099 byte TCP chunks after a greeting sent one byte at a time: 324992 samples, 0 split or swapped IQ pairs; a half greeting then close gives a clear error without a hang.

## Reviewed by reading, no change
- rtl_tcp, SpyServer (1 MiB body cap), TCI, KiwiSDR (stall timer, sequence gaps), RTL-SDR and HackRF glue: state caches are reset on reopen, gains and rate re-sent on reconnect.
- NXDN cipher flag already needs two agreeing messages. P25 flags come from RS-protected HDU/LDU/TSBK. TETRA treats one CRC-valid encrypted PDU as encrypted on purpose (unknown counts as encrypted, test_tetra_phy asserts it): not changed.
- EPIRB/COSPAS bit positions against C/S T.001; Schrader TPMS and Bresser decoders against rtl_433.
- ADS-B: local CPR decode from the receiver position can put one aircraft beyond 180 nm at an aliased position until the next even/odd pair; no clean fix without a range cap, left as is.

## Run (Release build, no sanitizer: the machine was saturated; ASan/UBSan sweeps belong on GitHub)
test_adsb_impair, test_acars_impair, test_ais_impair, test_hfdl_offset, test_p25_impair, test_ism_rx, test_sensors_rx, test_dstar_rx, test_nxdn_short, test_tetra_rx, test_ale_rx, test_sdrpp, test_rtltcp_chunks: all pass.

## Not covered
ASan/UBSan runs, GitHub sweeps, VDL2, EPIRB signal-level sweeps, hardware unplug mid-run on real radios, rtl_433 family-by-family comparison, ALE, trunking beyond P25.
