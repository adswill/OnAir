# CPU per second of signal, every mode (4.1, feat41-cpu)

Goal: find the modes that need more than one CPU core at their default sample rate (more than 1.0 CPU second per second of signal) and
make them cheaper without changing what they decode.

## How it is measured

- `test_qa_sweep --cpu` (tests/qa/test_qa_sweep.cpp, the all-modes QA sweep's generator and Engine child): for each mode its built-in
  test signal at the mode's default sample rate (60 dB, 16-bit file; a mode without a test signal: its transmitter's signal if it has one,
  otherwise noise only), then noise alone at the same rate and level (12 dB in-band noise level, no signal). Each file goes through a
  full Engine (file source, front end, receiver, spectrum off, sound off) as fast as it can run, 6 s of signal (capped at 160 M samples, so
  the X-band links get 0.5 - 4 s).
- The figure is CPU seconds per second of signal over the steady state: from a quarter of the signal to its end (start-up and table
  building left out). **All threads** is the whole process (getrusage); **busiest** is the single thread that used the most CPU in that
  window (Linux: /proc/self/task/*/schedstat; macOS: thread_info). Busiest is what one core must carry; all threads is what the computer
  pays. Work moved to more threads lowers the busiest figure but not the total, and is not counted as a saving here.
- `.github/workflows/cpu.yml` runs it in shards on GitHub's `ubuntu-24.04` (x86-64, 4 vCPU = 2 cores with SMT, AMD EPYC or Intel Xeon,
  a different processor each run) and `macos-15` (Apple silicon, 3 cores). With `compare` the same measuring program is built a second
  time over another ref's sources and run after it on the same runner: before and after figures are only compared within one run.
  `perf` runs one or more modes under `perf record` (receive run only) and prints the hottest functions and instructions. Until the file is
  on the default branch a branch asks for a run by pushing tools/perf/cpu-request.env. `tools/perf/cpu_table.py` merges the shards.
- Simulated signals only; a runner's figure depends on its processor (compare within a run).

## Before: the modes above one core (run 43364172327, integ-41 2b4d103df)

393 modes measured (every entry of the sweep's list: core standards, mode tunings and sub-modes). 40 are above 1.0 CPU s per s on at
least one OS, signal or noise alone. The full table (all 393 rows, worst first) is in the run's `cpu-table` artifact and job summary.
Figures: CPU s per s of signal, all threads / busiest thread; Linux runner AMD EPYC 9V74, macOS runner Apple silicon (macos-15, 3 cores).

| mode | Msps | Linux all | Linux busiest | Linux noise all | macOS all | macOS busiest | macOS noise all |
|---|--:|--:|--:|--:|--:|--:|--:|
| xb_landsatx | 300 | 25.65 | 23.35 | 5.67 | 20.78 | 19.57 | 2.22 |
| xb_metopsg | 140 | 15.34 | 14.38 | 1.94 | 15.05 | 14.05 | 1.41 |
| xb_oceansat3x | 120 | 12.30 | 11.37 | 1.88 | 10.89 | 10.22 | 1.06 |
| xb_kanopus | 90 | 8.39 | 7.69 | 1.38 | 6.93 | 6.51 | 0.92 |
| xb_gcomc | 90 | 7.02 | 6.43 | 1.52 | 6.42 | 5.78 | 0.96 |
| fy3mpt | 20 | 6.05 | 5.88 | 0.78 | 4.10 | 3.98 | 0.48 |
| xb_metopd | 45 | 6.03 | 5.65 | 0.71 | 5.57 | 5.32 | 0.46 |
| xb_probav | 45 | 5.14 | 4.84 | 0.78 | 4.06 | 3.85 | 0.43 |
| geonetcast | 52 | 4.90 | 1.71 | 1.58 | 3.79 | 1.28 | 1.17 |
| xb_jpss2 | 40 | 4.07 | 3.80 | 0.67 | 3.64 | 3.45 | 0.41 |
| arktikardas | 45 | 4.05 | 3.72 | 0.57 | 2.71 | 2.55 | 0.30 |
| isdbs | 48 | 3.87 | 3.54 | 1.17 | 3.27 | 3.12 | 1.40 |
| elektrordas | 45 | 3.37 | 3.08 | 0.50 | 2.86 | 2.66 | 0.37 |
| gk2auhrit | 30 | 3.19 | 0.94 | 0.95 | 1.86 | 0.63 | 0.79 |
| xb_npp | 25 | 3.04 | 2.84 | 0.39 | 2.12 | 2.03 | 0.22 |
| xb_terra | 45 | 2.90 | 2.61 | 0.61 | 2.08 | 1.93 | 0.38 |
| xb_saralx | 25 | 2.68 | 2.50 | 0.34 | 2.22 | 2.07 | 0.19 |
| xb_oceansat2 | 35 | 2.56 | 2.33 | 0.47 | 2.00 | 1.89 | 0.27 |
| dvbc2 | 10 | 1.88 | 1.78 | 2.53 | 1.08 | 1.01 | 1.65 |
| fy3ahrpt | 6 | 2.02 | 1.95 | 0.43 | 0.81 | 0.79 | 0.17 |
| kavb | 2.4 | 1.97 | 1.94 | 1.08 | 1.22 | 1.20 | 0.47 |
| xb_gcomw | 20 | 1.86 | 1.71 | 0.38 | 1.19 | 1.11 | 0.14 |
| wisun | 20 | 1.84 | 0.84 | 0.69 | 1.45 | 0.68 | 0.56 |
| dvbc | 10 | 1.75 | 1.10 | 0.22 | 1.53 | 0.86 | 0.19 |
| dvbsh | 8 | 1.14 | 1.06 | 1.72 | 0.80 | 0.76 | 0.99 |
| elrs | 8 | 1.63 | 1.55 | 1.63 | 0.86 | 0.82 | 0.86 |
| goesgrb | 20 | 1.62 | 0.54 | 1.06 | 1.19 | 0.47 | 0.88 |
| gk2ahrit | 10 | 0.96 | 0.88 | 1.57 | 0.66 | 0.63 | 1.38 |
| npr | 4 | 1.55 | 1.51 | 1.55 | 0.78 | 0.75 | 0.78 |
| gk2acdas | 8 | 1.25 | 1.18 | 1.41 | 0.66 | 0.64 | 0.75 |
| msgraw | 6 | 0.61 | 0.57 | 1.38 | 0.54 | 0.51 | 0.79 |
| fpv | 20 | 0.65 | 0.54 | 1.34 | 0.41 | 0.36 | 0.86 |
| dtmb | 10 | 1.34 | 0.55 | 0.33 | 0.89 | 0.45 | 0.28 |
| halow | 6 | 1.11 | 1.05 | 0.70 | 1.31 | 1.27 | 0.85 |
| xb_aqua | 15 | 1.29 | 1.19 | 0.29 | 1.17 | 1.08 | 0.16 |
| sband | 10 | 1.24 | 1.17 | 1.24 | 0.73 | 0.65 | 0.84 |
| himawaricast | 6 | 0.61 | 0.25 | 1.24 | 0.41 | 0.18 | 0.96 |
| isdbt | 8 | 1.14 | 0.61 | 0.86 | 0.70 | 0.37 | 0.38 |
| ism | 2 | 0.97 | 0.94 | 0.06 | 1.02 | 1.01 | 0.07 |
| atsc | 8 | 0.84 | 0.39 | 0.42 | 1.00 | 0.45 | 0.60 |

## What changed (feat41-cpu)

All but one change give the same output bit for bit; the exception is DVB-C on Linux x86, below.

| change | modes | how | output |
|---|---|---|---|
| NPR sync search: the 16 network correlations side by side, patterns less their mean worked out once, the root only when one can reach 0.86 (a frame needs 0.93) | npr | core/src/xlink/npr.cpp | same hits, metric and network (exact) |
| Inverted-IQ ExpressLRS bank takes conj of the normal bank's channel instead of a second channel filter on conj(x) at -offset | elrs | Bank::feedConjOf (loraraw_bank.h), Channelizer::lastOut (mesh_lora.h, additive), rc_rx.cpp | bit for bit: new check in rclink_rx compares both ways over 3 rates x 6 offsets x random chunk sizes, Linux and macOS |
| popcount without a libgcc call on x86 without POPCNT (the release baseline): bit-parallel count | sync searches of FY-3, HRPT, Meteor, X band, wxsat (RDAS ...), GOES / GK-2A | bits::popcount (common/bits.h); the receivers' `__builtin_popcount` calls now use it | same counts (integer) |
| FastSinCos::polar: ifloor (simd.h) instead of std::floor | X band, wxsat, FY-3 (and the BLE / Wi-SUN generators) | dsp/fast_sincos.h | same value |
| DVB-C AVX2/FMA copy chosen by CPU features: `target_clones("arch=x86-64-v3")` (GCC 12+) instead of `arch=haswell`, which GCC picks by CPU model, so AMD and newer Intel ran the baseline copy | dvbc (Linux x86 only) | core/src/dvbc/dvbc_rx.cpp | FMA rounds differently (as on Haswell before): every delivered packet count and error count in dvbc_rx (20 cases, 0 bad), dvbc_j83b, dvbc_docsis the same; dvbc_j83b_rf RS-clean blocks up (28493 -> 28793, failed the same) |
| DVB-C2 carrier-base search (no centre frequency known): doubled sign table instead of a modulo per term, eight n summed side by side (each in the old order) | dvbc2 | dvbc2_rx.cpp findKBase | exact (dvbc2 test output identical) |
| DVB-SH turbo SISO: branch metrics once per period, the inner periods without per-state checks (max* is symmetric) | dvbsh | dvbsh_fec.cpp | exact (dvbsh_fec / chain / tdm / engine output identical) |
| S-band / GRB / UHRIT / CDAS FIRs in eight running sums | sband, goesgrb, gk2auhrit, gk2acdas | sd3_demod.h | sum order changes; sd3_rx output identical on Linux and macOS (92 lines: frames, RS, packets, files at every SNR and impairment case) |
| GOES ring without a modulo per sample | goes, gk2ahrit, gk2alrit ... | goes_rx.cpp | exact |

Tried and taken back: the GOES / GK-2A filter in sixteen running sums (20 % cheaper on GK-2A HRIT, but goes_sweep's 40 dB fade case gave
277 frames instead of 279); the Wi-SUN channel filter in eight running sums (no measurable saving).

## After (Linux, run 43381677385, AMD EPYC 9V74: base = integ-41 receivers with this branch's measuring program, same runner)

CPU s per s of signal, all threads / busiest thread; before -> after. Note: this run's runner differs from the first one, so its base
figures differ from the table above.

| mode | Msps | signal: all | signal: busiest | noise: all | noise: busiest |
|---|--:|--:|--:|--:|--:|
| npr | 4 | 1.542 -> 0.745 | 1.503 -> 0.714 | (no test signal: noise) | |
| elrs | 8 | 1.379 -> 0.888 | 1.313 -> 0.832 | (no test signal: noise) | |
| dvbc | 10 | 1.470 -> 1.251 | 0.915 -> 0.744 | 0.170 -> 0.175 | 0.111 -> 0.112 |
| dvbc2 | 10 | 1.853 -> 1.855 | 1.766 -> 1.766 | 2.527 -> 1.577 | 2.440 -> 1.499 |
| dvbsh | 8 | 0.929 -> 0.802 | 0.875 -> 0.749 | 1.421 -> 1.447 | 1.357 -> 1.385 |
| sband | 10 | 1.232 -> 1.107 | 1.165 -> 1.036 | 1.244 -> 1.135 | 1.175 -> 1.059 |
| goesgrb | 20 | 1.613 -> 1.622 | 0.529 -> 0.539 | 1.125 -> 0.982 | 0.886 -> 0.838 |
| gk2auhrit | 30 | 3.147 -> 3.167 | 0.952 -> 0.949 | 0.971 -> 0.879 | 0.763 -> 0.724 |
| gk2acdas | 8 | 1.087 -> 1.082 | 1.033 -> 1.028 | 1.197 -> 1.173 | 1.142 -> 1.118 |
| fy3mpt | 20 | 6.049 -> 5.539 | 5.874 -> 5.381 | 0.781 -> 0.741 | 0.670 -> 0.631 |
| fy3ahrpt | 6 | 1.598 -> 1.558 | 1.548 -> 1.509 | 0.377 -> 0.357 | 0.336 -> 0.321 |
| gk2ahrit (GOES filter in parallel sums, taken back) | 10 | 0.786 -> 0.632 | 0.724 -> 0.574 | 1.275 -> 1.117 | 1.208 -> 1.053 |
| xb_probav / xb_metopd / xb_npp / xb_gcomw / xb_aqua | 15-45 | within +-5 % (runner noise) | | | |
| dvbs (unchanged control) | 10 | 0.846 -> 0.842 | 0.331 -> 0.330 | 0.305 -> 0.306 | 0.248 -> 0.249 |

**macOS (same run, macos-15).** This runner was not steady: modes whose code is unchanged on ARM (the X-band links, RDAS, HRPT, Meteor:
popcount and floor compile to the same instructions there) read 10 - 43 % higher on this branch, which ran first. Read with that bias, the
changes that act on ARM still come out lower: npr 0.835 -> 0.639, elrs 1.098 -> 0.829 (signal, all threads), dvbc2 on noise 1.809 ->
0.919, gk2ahrit 0.964 -> 0.679 (with the GOES filter change that was taken back), sband 0.681 -> 0.651. A second macOS run is needed
for figures to quote.

The popcount, floor and DVB-C clone changes only act on x86 (ARM has the instructions and no clones); the others on both.

## Quality checks (GitHub quick.yml, this branch and integ-41 on the same runner, verbose output compared line by line)

- Linux run 43381497262 (36 tests): fy3_proto fy3_rx fy3_mode hrpt_* meteor_* xband_rx xband_mode wxsat_rx wxsat_mode wxsat_filecast
  goes_* gk2a_rx gk2a_engine dvbc_* sd3_rx dvbc2 dvbsh_*: all pass on both. Output identical for every receiver test except dvbc_rx /
  dvbc_j83b_rf (the FMA copy: MER and the RS-clean counter move, delivered and bad packets identical) and goes_sweep's fade case (the
  GOES filter change, since taken back). The *_engine tests run as fast as the runner allows and report a time-dependent number of
  frames: they differ between two runs of the same code (meteor_engine, whose receiver only got the exact popcount, moved 72 -> 70).
- macOS run 43381652395 (17 tests: sd3, goes, gk2a, wisun, dvbsh, dvbc2): all pass; identical output but goes_sweep's fade case (taken back).
- Linux and macOS runs 43373063226 / 43373078405: xlink_npr, rclink_rx (with the new bit-for-bit check), rclink_39, rclink_proto,
  loraraw_rx, loraraw_engine, mesh_rx, mesh_phy, mesh_bank_eof, mesh_impair: pass.
- Run 43390123519: goes / gk2a / wisun after taking the two filter changes back.
- No sensitivity sweep was needed for the exact changes (same output bit for bit). The sd3 change was checked by sd3_rx's own SNR and
  impairment cases (identical frame counts); fy3ahrpt, the one changed mode in the sens-quick baseline, changed only exactly.
- Simulated signals only.

## Left above one core, and why

- **X-band links** (xb_landsatx 300 Msps ... xb_aqua 15 Msps), **FY-3 MPT** (20 Msps), **ISDB-S** (48), **Elektro / Arktika RDAS** (45),
  **GEONETCAST** (52, LDPC on worker threads), **GK-2A UHRIT** (30), **GOES GRB** (20): the cost grows with the sample rate; the per-sample
  carrier and timing loops (NCO, matched filter, interpolator, a strobe every 0.7 samples) are serial by nature and already table-driven
  (3.9 feat39-qafixB / widecpu). A profile of xb_probav spreads the time over the whole loop with no single hot spot. Below one core at
  these rates would need about 3 ns a sample (300 Msps) or a different receiver structure (block-wise demodulation), not a local change.
  They are used on recordings, which play at whatever speed the computer manages.
- **DVB-C2 on noise** (1.58 after, 2.53 before): the carrier-base search runs again and again on noise; it is now about 40 % cheaper, but
  a gate that skips it would change when a weak signal is found. Clean: 1.85, ClockCorrector and LDPC.
- **DVB-SH on noise** (1.42): TDM slot search and `hypotf` (a correctly rounded libm call); replacing it with sqrt changes bits.
- **GK-2A HRIT / CDAS, S-band on noise**: the frame sync search (GOES syncSearch 41 % on noise).
- **DVB-C** (1.25 all threads, 0.74 busiest after): equaliser and back end on two threads.
- **Wi-SUN, DTMB, ISDB-T, ATSC 1.0** above 1.0 only as the sum of their threads (busiest 0.4 - 0.8).
- **KAVB, FY-3 AHRPT, HaLow, ISM**: around 1.0 - 2.0 on the Linux runner, under 1.3 on Apple silicon; not changed here.
