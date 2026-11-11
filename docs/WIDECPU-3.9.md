# Wide downlinks on noise (3.9, feat39-widecpu)

What changed, the measurements and the checks are in docs/QA-SWEEP-3.9.md, section "The other wide downlinks (feat39-widecpu)".
This page says how they were measured, so that they can be repeated.

- Test files: the sweep's own (`test_qa_sweep --gen <mode> <rate> <secs> 12 <dir>`): `clean.ci16` and `noise.ci16` (white noise in the
  signal's 99 % band, no signal).
- Engine figures: `test_qa_sweep --run <mode> <rate> <secs> <file> <cond>`, its `cost` (CPU s, all threads, per second of signal after
  the first quarter).
- Receiver figures: a small program that reads the CI16 file (scaled by 1/32768), feeds the receiver in blocks of 65536 and times
  `feed()` with the process CPU clock; at the end it hashes what was decoded (ISDB-S: the transport packets; FY-3: frame counters,
  virtual channels, picture rows; wxsat: counters and `saveResults()`). Before = the receiver built from integ-39 (4a38e3ea3), after =
  this branch's sources, in the same program.
- Profiles: thread CPU timers (`CLOCK_THREAD_CPUTIME_ID`) around stages, and the 24 MHz counter for scopes that run per symbol;
  no sampling profiler.
- Noise CPU of the other modes: the same `--run` on each mode's `noise.ci16` (237 modes; ATSC 1.0 and the X-band links left out).
- New counters: `idleInputSamples()` on `IsdbsReceiver`, `Fy3Receiver`, `WxsatReceiver` and `DvbsReceiver`.

Simulated signals only: no off-air recording of these downlinks was available.

## The custom_bandwidth_all segfault at GeoNetCast (separate fix)

`custom_bandwidth_all` crashed at GeoNetCast in a DVB-S2 worker thread (`onair-dvbs-w`). Not caused by a custom width or rate: it is
the test's many engine starts and stops. Reproduced (2 of 3 runs plain; every run under AddressSanitizer with the wxsat and file-layer
code instrumented): heap-use-after-free in `FilecastLayer::pushTs`, called by `S2Rx::emitPackets` on the worker, of the file layer
freed by `WxsatReceiver::Impl::~Impl`. The DVB-S2 receiver's packet callback holds a pointer to the file layer, and the members were
destroyed in reverse order of declaration: `files` before `dvb`, whose destructor is what stops the workers. Fix: `~Impl()` resets
`dvb` first (`core/src/wxsat/wxsat_rx.cpp`). New test in `test_wxsat_filecast`: a GeoNetCast receiver destroyed with frames in flight,
six times. ASan before: heap-use-after-free; after: clean, and `custom_bandwidth_all` with `ONAIR_TEST_MODES=geonetcast` no longer
crashes (0 of 6 runs, 6 of 6 before). HimawariCast shares the path and the fix.
