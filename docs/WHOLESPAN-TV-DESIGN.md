# Whole-span TV for every standard (design, 0.3.0)

Status: design only, nothing built. Goal: one wide capture, one receiver per occupied signal, for DVB-T/T2 (exists), DVB-S/S2/S2X, DVB-C/C2,
ISDB-T, ISDB-S, ATSC 1.0/3.0, DTMB, analogue TV, and the synthetic wideband source.

## 0. What exists today

- `core/include/dect2/engine/wide_bank.h`: `WideBank<Rx,Tel>` (narrowband modes, DMR etc.). `CarrierFinder` (dsp/carrierfind.h) finds carriers;
  per carrier a `Channelizer` + the mode's own `Rx`; `feed()` runs the live carriers in parallel on `ThreadPool::shared()`; `WideOps` hooks
  (`outRate`, `occupiedHz`, `setup`, `audio`, `info`, `freqCorrection`, `rowState`); `WideCarrier` rows; `sel_` / `autoPlay_` / `effSel()` choose
  the played carrier; `retire()` keeps the log of vanished carriers. Fixed 48 kHz-class assumptions (audio, call lists) make it unfit for TV as is.
- `core/include/dect2/engine/multitv.h`, `core/src/engine/multitv.cpp`: `MultiTv` for DVB-T/T2 only. Raster-based: `spanSlots()` gives slots of
  one channel width; `SpanScanner` (4096-bin averaged PSD, `floorDb()` = quietest 250 kHz, `measure()` = per-slot median vs floor, threshold 6 dB)
  decides occupancy; per occupied slot `Slot` = `ChannelFilter` (mix + `ExactResampler`) + `T2Receiver` or `DvbtReceiver` + `BbUnpacker` + `TsDemux`
  + own thread and block queue (`kQueueSec` 0.2 s, drops when behind, `dropped` counter); absent for `kAbsentEvals` (8 x 0.25 s) before stop.
  Selected slot's packets go to one `Sink`; the others keep their own service list/EPG (`services()`, `epg(slot)`, `snapshot(slot)`).
- `core/src/engine/engine_multitv.cpp`: `Engine::setMultiChannel(on,max,grid)`, `selectTvChannel`, `tvChannels()` -> `TvChannelRow`
  (`channels/tvchan.h`), `multiServices()`, `syncMultiTv()`, `onMultiPackets()`, `defaultMaxChannels()` (3 on >=8 cores else 2),
  `multiChannelSupported(stdMode)` (engine.h:306: only DVB-T2 and DVB-T), UI in `app/panels/tv.cpp`.
- Source side: `core/include/dect2/dsp/synth_wide.h` (pub/synth-wideband2-030) stitches 2-5 radios into one wide stream "hand to the channelizer /
  WideBank". Its own header says wide OFDM across seams is unmeasured: a signal must sit inside one slice or be tolerant.

Two things are missing for the other standards: (a) channel positions are a fixed raster (DVB-T width x n), but satellite, cable and ISDB-S
signals are at arbitrary frequency with arbitrary bandwidth; (b) the receiver is hard-wired (`T2Receiver`/`DvbtReceiver`, `BbUnpacker`).

## 1. Architecture: one `SpanTv` layer

New files under `core/include/dect2/engine/spantv/` (names proposed):

```
wide capture (cf32, spanRate)
  -> SpanAnalyzer      (PSD, floor, edges -> std::vector<SignalCand>)             spantv/span_analyzer.h
  -> SignalTracker     (candidates over time -> stable SignalId, hysteresis)      spantv/signal_tracker.h
  -> BlindClassifier   (standard + parameters when user did not fix them)        spantv/classify.h
  -> ReceiverPool      (budget, priority, start/stop, queues, threads)            spantv/pool.h
       per signal: ChannelFilter(offset, outRate) -> TvAdapter::feed -> TS packets -> TsDemux -> Sink(selected) / per-signal service list
```

MultiTv becomes `SpanTv` with `MultiTv` kept as a thin wrapper (phase 1) so the engine, tests and UI keep working.

### 1.1 Occupied-signal detection (`SpanAnalyzer`)
Generalise `SpanScanner` (keep it; the raster path stays for DVB-T/ATSC/ISDB-T/DTMB where the raster is known and cheap):

1. PSD: reuse its 4096-bin average, but make bins/averaging configurable: satellite needs a finer bin (rate/4096 = 2.4e6/4096 = 586 Hz is fine;
   at 40 Msps it is 9.8 kHz, enough for >=1 Msym/s transponders). Keep a slow average (noise floor) and a fast one (appear/disappear).
2. Noise floor: today `floorDb()` = quietest 250 kHz window. Replace by a robust estimate: the lower 20th percentile of the smoothed PSD after
   removing a fitted slope (LNB IF tilt of 5-10 dB across 950-2150 MHz and the radio's roll-off). Output `floor(f)` as a curve, not one number.
3. Edge detection: threshold `floor(f)+thresholdDb` with 2 dB hysteresis, then for each run measure the -3 dB and the "-X dB above floor"
   edges by walking out from the peak; occupied bandwidth = distance between the edges at half the plateau height (flat-top signal: DVB/ISDB/ATSC/DTMB/cable
   have a flat spectrum, which makes this accurate; FM analogue TV does not, see 2.9). Merge runs separated by < 2 bins. Reject runs narrower than
   the minimum signal of the enabled standards (carrier-only spikes, tones: peak/width ratio test, LNB beacon).
4. Satellite symbol rate: Rs = BW / (1+roll-off). Roll-off from the spectral shoulder: the edge slope between -1 dB and -20 dB is about
   0.35/0.25/0.20/0.15/0.10/0.05 times Rs wide (DVB-S 0.35, S2 0.35/0.25/0.20, S2X 0.15/0.10/0.05); pick the nearest, then refine Rs in the
   receiver's own symbol-timing recovery (the receivers already do blind search: `dvbs_rx.h`, see 2.1). The analyzer only has to be within +-5 %.
   A cyclic-spectrum check (|x|^2 spectral line at Rs, i.e. the PSD of |x|^2 has a line at Rs) narrows it further at cost of one more FFT per candidate.
5. Output: `SignalCand { double centerHz, bwHz, peakDb, snrDb, edgeSlopeHz; double rsEstimate; uint32_t stdMask; }`.

### 1.2 Tracking and hysteresis (`SignalTracker`)
Same rules as MultiTv: association by centre within max(bw/4, 1 bin), a signal is "present" after 2 consecutive evaluations, "absent" after
`kAbsentEvals` (8 x 0.25 s); a running receiver that has TS lock is never dropped for a short fade. Stable `SignalId` (monotonic int) replaces the
raster `slot`; for raster standards `SignalId` can still equal `slot` so `TvChannelRow.slot` stays valid.

### 1.3 Blind detection of the standard
When the user has not fixed one (Auto) or the band plan is ambiguous. A cascade, cheapest first, run once per new candidate on a 100-300 ms snapshot
cut out by the channeliser (not on the whole span):

| Test | Distinguishes |
|---|---|
| bandwidth + flat top + edge slope | 5.7/6/7/8 MHz OFDM or single carrier vs satellite (BW 1-60 MHz, no fixed raster) |
| CP autocorrelation peaks (OFDM) at lags Tu: 1/4..1/32 of FFT size | DVB-T/T2/ISDB-T/DTMB(no CP: PN) vs single-carrier; FFT size gives 2K/8K/32K |
| ATSC 1.0 pilot: line 310 kHz above lower edge, 6 MHz, 8VSB | ATSC 1.0 (existing `atsc_rx.h` sync) |
| T2 P1 / ATSC3 bootstrap correlation | DVB-T2 (`t2rx.h`), ATSC 3.0 (`atsc3` bootstrap) |
| NTSC/PAL vision carrier + sound subcarrier lines, 15.6/15.7 kHz line rate | analogue (`atv_std.h`) |
| single carrier: SRRC matched-filter bank at the estimated Rs, then DVB-S2 PL header (SOF 26 sym + PLSCODE) correlation, DVB-S sync-byte search, DVB-C QAM constellation order from the moment test | DVB-S / S2 / S2X / DVB-C |
| ISDB-S TMCC pattern (`core/src/isdbs/isdbs_slot.cpp`) | ISDB-S |

Each adapter exports `probe(const cf32* x, size_t n, double rate, SignalCand&) -> float score` (0..1) and the classifier takes the best
above 0.5, otherwise tries again 2 s later (max 5 tries, then "unknown signal" row, no receiver). The result is cached per (centre, bw) so a
signal that returns does not need classifying again.

### 1.4 Channelisation
`ChannelFilter` (mix with 4096-entry phase table + `ExactResampler`) stays. Changes:
- `outRate` per standard comes from `TvAdapter::nativeRate(const SignalCand&)` (today `tvChannelRate(widthMhz,spanRate)` = 1.25 x width).
  Satellite: 2-4 x Rs (what `dvbs_rx.h` front end wants; use 2 x Rs x 1.25 = 2.5 x Rs to leave filter transition room).
- Cost: ChannelFilter is O(spanRate) per signal for the mixer + the resampler's long FIR at the high rate side. With N signals in a 40 Msps
  span that is N x 40 Msps of mixing. Phase 2 optimisation: a two-stage channelizer (shared polyphase filterbank cutting the span into
  M equal sub-bands first, e.g. 8 x 5 Msps; then a per-signal ChannelFilter from the sub-band). Not needed at <= 4 signals; measure first
  (docs/PERFORMANCE-DSP.md has `tv_channel`, `exact_resampler` rows; per-signal numbers: unknown, to be measured with `bench_dsp`).
- Overlapping/adjacent signals: the resampler's filter cutoff is the signal's own bandwidth x 1.1, so a neighbour at 0.1 x BW of gap leaks
  in; accept (receivers tolerate adjacent-channel at 30 dB); flag in the table as "adjacent interference".

### 1.5 Receiver pool (`ReceiverPool`)
Generalises `MultiTv::live_` + `Slot` thread/queue (keep that code, it is already hardened: queue limit, drop counting, `wait()` for tests).
- Budget: measure CPU live. Each running receiver already reports `cpuPct` (time spent / signal time handled, `TvChannelRow.cpuPct`).
  Pool total = sum(cpuPct)/100 in cores; allowed = 0.7 x `hardware_concurrency()` minus the analysis/GUI reserve (1 core). A new receiver starts only if
  predicted cost (adapter's `costPerSecond(cand)`, a table seeded from PERFORMANCE-DVB.md and corrected by the measured value after 2 s) fits.
  `maxChannels` stays as a hard cap and a user setting; "Auto" = budget-limited.
- Priority order: (1) selected signal, (2) signals with an active recording/stream/network-tuner client, (3) previously locked signals,
  (4) strongest SNR, (5) others. When over budget, lowest priority is stopped first, never the selected one. Also stopped under battery mode
  (perf-power-030 hook: max = 1 on battery unless the user set otherwise).
- Hysteresis: start threshold SNR over floor >= 6 dB, stop at < 3 dB for 2 s; minimum run time 5 s; a stopped-for-budget signal is not retried for
  10 s (avoid thrash); a receiver that never locks in 15 s (satellite: 30 s because of blind search) goes to "no lock" and is retried every 60 s.
- Threads: one thread per receiver as today (`Slot::qMu/qCv/q`), because the DVB receivers have their own pipelined stages
  (DVB-T decoder thread, T2 resampler thread). Do NOT move them to `ThreadPool::parallelFor` like `WideBank`: a TV receiver's feed()
  is 0.3-1.2 s of CPU per second, too coarse for the pool's fork-join.
- Dropping: a slow receiver loses blocks (live radio) or blocks `feed()` (`blocking=true`, files and tests), as now.

### 1.6 Adapter interface (`TvAdapter`)
```cpp
struct TvAdapter {
    virtual ~TvAdapter() = default;
    virtual int  stdId() const = 0;                                   // matches Engine stdMode_
    virtual float probe(const cf32*, size_t, double rate, SignalCand&) = 0;   // blind detection, 0..1, fills params it can
    virtual std::vector<ChannelSlotPos> raster(double spanRate, const SpanTvOpts&) = 0;   // empty = free-frequency standard
    virtual double nativeRate(const SignalCand&) const = 0;           // what the receiver wants
    virtual double minBwHz() const = 0; virtual double maxBwHz() const = 0;
    virtual float  costPerSecond(const SignalCand&) const = 0;        // CPU s per s of signal, seed values in 2.x
    virtual std::unique_ptr<TvRx> make(const SignalCand&) = 0;
};
struct TvRx {                       // one receiver, one signal
    virtual void feed(const cf32*, size_t) = 0;
    virtual void setTsSink(std::function<void(const uint8_t* pk, size_t n)>) = 0;   // after BB unpack where the standard has BBFRAMEs
    virtual TvRxStatus status() const = 0;   // state, snr, cpu, ber, params (mod/code/Rs/PLS, TS id, network)
    virtual void setPipelined(bool) = 0;
    virtual double carrierOffsetHz() const = 0;   // refinement fed back to the pool (like WideOps::freqCorrection)
};
```
Adapters for DVB-T/T2 wrap the existing `T2Receiver`/`DvbtReceiver` + `BbUnpacker` exactly as `MultiTv::Slot` does. The new adapters wrap the
existing single-signal receivers: `dvbs_rx.h`, `dvbc_rx.h`, `isdbt_rx.h`, `isdbs_rx.h`, `atsc_rx.h`, `atsc3` receiver, `dtmb_rx.h`, `atv_rx.h`.
Analogue TV has no TS: `TvRx` also offers `pullVideo()` / audio path; the pool treats it as a different sink kind (see 2.9).

## 2. Per standard

Cost figures: PERFORMANCE-DVB.md (pub/perf-dvb-030), "CPU s per s of signal", Apple Silicon, shared machine, so indicative only; x86/ARM
phones are slower, expect 1.5-3x. The figure is for the receiver at the radio rate given, not for the channeliser.

### 2.1 DVB-S / S2 / S2X (first priority)
- Plug-in: `dvbs_rx.h` (S2Rx::fastRun, SymbolTimer, s2RefinePhase, PolyResampler); `dvbs_testkit.h` + `dvbs_gen.h` + `dvbs_tx.h` generators.
  Free-frequency standard: no raster; `SpanAnalyzer` gives centre + BW + Rs; adapter sets the receiver's start symbol rate from that
  (the receiver's blind Rs search then only needs a narrow window, which makes lock much faster than today's full blind scan).
- Cost: S2 8PSK 3/4 10 MBd 1.03 (all threads), 32APSK 4/5 8 MBd 1.21, DVB-S QPSK 10 MBd 0.52 (baseline). Scales roughly with symbol rate and LDPC rate:
  a 27.5 MBd transponder is ~2.5x the 10 MBd figures, i.e. a single big transponder can use 2-3 cores; narrow ones (1-5 MBd; the priority case) cost 0.1-0.5 core each.
  Typical RTL-SDR at 2.4 Msps: bandwidth usable ~2.0 MHz after the filter roll-off -> 1 transponder up to ~1.6 MBd (x 1.25 roll-off) or 2-3
  at 0.5-0.7 MBd (SCPC/feeder/FTA small carriers). A 2.4 Msps stream holds at most 2-3 carriers of 0.6 MBd, so for RTL-SDR the target is SCPC.
- Minimum capture rate: each carrier needs BW = Rs(1+alpha); the span needs sum of BW + guards (one Rs). 10 Msps radio: 3 x 2 MBd or 1 x 8 MBd;
  20 Msps (Airspy/HackRF/Lime): 1-2 big or 6-8 small; 40+ Msps (USRP/Lime/BladeRF): a whole 36-72 MHz transponder group of one polarisation/band slice.
  The 950-2150 MHz IF band is 1200 MHz: no SDR holds it at once; the pool is per tuned slice and the UI offers "next slice" (step by the usable span).
- LNB / IF handling:
  - The span centre is an IF frequency; conversion to the satellite frequency needs the LNB LO (universal: 9750 low / 10600 high; C-band 5150; user-defined).
    `SpanTv` takes an `LnbCfg` {loLow, loHigh, switchHz 11700, inverted(C-band)} and every row shows both IF and sky frequency + the polarity (V/H) set by the user.
  - 22 kHz tone and 13/18 V: the radio/bias-tee supplies the voltage, tone is hardware (RTL-SDR boards with bias-T only supply 5V - need an LNB power inserter;
    most SDRs cannot do 13/18 V or 22 kHz). The app only stores the user's choice (band/polarisation) in the profile and warns when the hardware
    cannot switch. DiSEqC: not generated by SDRs; only for devices that have it (a set-top-box-style tuner via the driver); document, do not promise.
  - Spectral inversion: low-side LO makes the spectrum inverted for some LNBs/converters (C-band and some Ku): the adapter has a try-both (`inverted`) option,
    chosen by the receiver's lock; remember the result per LNB.
  - IF tilt across the span: 5-10 dB; the per-frequency floor in 1.1 handles it; the receiver's AGC after the channeliser (add a fast AGC stage in the
    ChannelFilter output - the existing path has none because an OFDM channel is flat).
  - Strong neighbours: transponder level varies 10-20 dB; adjacent carrier leakage limited by the resampler's stopband (ExactResampler windowed sinc: confirm >= 50 dB; measure).
- 8PSK/APSK: the receivers handle them already; the problem is dynamic range (16/32APSK needs ~12+ dB SNR and a linear 10-bit front end; RTL-SDR 8 bit gives QPSK/8PSK low rates only).
  Mark rows "needs >= X dB SNR" from MODCOD.
- VCM/ACM and multistream: S2 with VCM changes MODCOD per frame; the receiver follows PLS per frame (existing). Multistream (MIS, ISI per stream; T2-MI is an ISI too): TS output is per ISI; the pool
  treats each ISI as an extra "logical signal" under one physical one: one receiver, N TS sinks (`ISI` column, and "decode all ISIs" off by default because LDPC/BCH
  runs for every ISI's frames; selecting one ISI makes the receiver discard other frames before FEC - cheap). T2-MI-in-S2: existing T2-MI decapsulation feeds `T2Receiver`'s TS stage.
  GSE/IP (`dvbs_ip.h`) streams show as "IP" rows, not TS services.
- Tests: signal generators `dvbs_gen.h`/`dvbs_tx.h` (QPSK..32APSK, S2X roll-offs) and `dvbs_testkit.h`; new multi-carrier generator = sum of several `dvbs_tx` streams at
  different frequency offsets and symbol rates with a tilt and noise (tests/dvbs/test_dvbs_span.cpp).

### 2.2 DVB-C / C2
- Plug-in: `dvbc_rx.h` (single-carrier QAM). Cable plan is a raster (8 MHz Europe, 6 MHz US; 7/8 MHz bandwidth at 6.9-6.95 MBd) so it works as raster path
  (`adapter.raster()` returns the plan's channels from `core/include/dect2/channels` / `bandplan`), with the free-frequency path for odd Rs.
- Cost: 256-QAM 6.95 MBd at 10 Msps = 1.13 (baseline, not yet optimised). Heaviest per MHz of all single carriers: the optimisation that DVB-T/S2 got (perf-dvb-030) should be done in phase 4 before enabling >2 channels.
- Min capture: 8 MHz per channel; a 20 Msps radio holds 2, 40 Msps holds 4-5.
- Specific: QAM order and Rs differ per channel; blind QAM order detection at lock (cheap). DVB-C2 (OFDM, 4K FFT, data slices, 6/8 MHz up to 450 MHz wide): no C2 receiver in the tree - out of scope until one exists; note only.
- Tests: a DVB-C generator exists (`dvbc.h` has tx? confirm; if not, write a minimal one in tests with `dvbc` mapper) - mark as TODO check in phase 4.

### 2.3 ISDB-T
- Plug-in: `isdbt_rx.h`; generator `isdbt_gen.h`. Raster standard: 6 MHz (Japan 6, Brazil/Latin America 6), 13 segments; also 1-seg
  (partial reception, centre segment, 430 kHz) that can ride inside the same channel: one receiver gives both the full-seg and 1-seg TS; do not start a second receiver for it.
- Cost: 8 Msps = 0.57 (baseline). Cheap.
- Min capture: 6 MHz per channel x 1.25 -> 8 Msps radio holds one, 20 Msps holds 3.
- Specific: hierarchical layers A/B/C with different modulation: TS from all layers merged by the TMCC remux (existing). Mode 1/2/3 and GI blind detection via the CP test in 1.3.
- Tests: `isdbt_gen.h`; multi-channel generator = N generators at 6 MHz spacing summed (same pattern as the existing DVB-T multi-channel test of MultiTv).

### 2.4 ISDB-S (branches pub/mode-isdbs-030, isdbs-spec2-030)
- Plug-in: `isdbs_rx.h`, generator `isdbs_gen.h`/`isdbs_synth.cpp`, slot logic `isdbs_slot.cpp`. ISDB-S (BS digital, 12 GHz band, 34.5 MHz per transponder, 28.86 MBd TC8PSK/QPSK/BPSK mixed in slots by TMCC): a free-frequency satellite signal that sits in the same IF span as DVB-S2; BS channels are on a 38.36 MHz raster, so adapter.raster() is
  useful (known BS channel numbers). 2nd-gen ISDB-S (spec2) is wideband (up to 33.7 MHz with MIS) and shares the receiver frontend in these branches.
- Cost: unknown (not in PERFORMANCE-DVB.md); expected like DVB-S2 8PSK at 28.86 MBd ~ 2-3x of 1.03 => 2-3 cores for one transponder.
- Min capture: >= 40 Msps for one transponder with roll-off 0.35 (about 39 MHz); only wide radios (Lime, USRP, BladeRF, AirspyHF? no) can hold it; can hold two on 100 Msps.
- Specific: classification must separate it from DVB-S2 (same IF) with the TMCC pattern test; relative TS (MPEG-2 TS with TLV for spec2).
- Tests: `isdbs_gen`/`isdbs_synth` for one transponder; multi = two shifted copies.

### 2.5 ATSC 1.0
- Plug-in: `atsc_rx.h`, generator `atsc_gen.h`. 6 MHz raster (US channels 2-36 UHF + VHF, the band plan in channels/).
- Cost: 8 Msps = 0.90 (baseline).
- Min capture: 6 MHz per channel (8 VSB, 10.76 MBd; 6 MHz carrier including the filter). 8 Msps radio holds 1; 20 holds 3.
- Specific: pilot offset: the 8VSB pilot is 309.440559 kHz above the lower channel edge, so the signal energy is not centred on the raster centre. The raster must be centred on the channel centre, but a mixer that centres on the pilot would be 2.69 MHz off. Use the pilot-finder (the receiver already finds it) as the `freqCorrection` feed-back (as `WideOps::freqCorrection`) and let the PSD's edge detection find the 6 MHz run directly; NTSC co-channel (pilot at 1.25 MHz) is a possible neighbour in analogue-plus-digital markets: classifier's NTSC test removes false positives.
  Also DVB-T 6 MHz and ISDB-T share the 6 MHz raster in the Americas: classifier needed when mode is Auto.
- Tests: `atsc_gen.h`; multi = N generators at 6 MHz, optionally with a pilot offset in the sum.

### 2.6 ATSC 3.0
- Plug-in: the ATSC3 receiver (`core/include/dect2/atsc3/`; `atsc3_synth.h` is the transmitter/synthesiser). 6 MHz raster (up to 6/7/8).
- Cost: 10 Msps = 0.40 (baseline).
- Min capture as ATSC 1.0.
- Specific: bootstrap (4 symbols, fixed) gives detection; the output is ALP / LLS / ROUTE, not a plain TS (ALP to TS or to IP via `atsc3_remux`); the sink for a non-selected signal is a service list from LLS/SLT + ESG, not TS packets. The `TvRx` interface therefore needs a second sink kind ("ALP/IP streams"); phase 5 decides whether the shared services list takes ATSC3 services through `TsService`-like records (see 2.10).
- Tests: `atsc3_synth.h` (+ tests/atsc3/atsc3_sim.h); multi = N shifted.

### 2.7 DTMB
- Plug-in: `dtmb_rx.h`; generator `dtmb_gen.h`/`dtmb_tx.h`/`dtmb_testkit.h`. 8 MHz raster (China 8, HK 8), single carrier or multi-carrier (C=3780) mode.
- Cost: 10 Msps = 0.49 (baseline).
- Min capture: 8 MHz x 1.25.
- Specific: PN modes 420/595/945 (frame head), single carrier vs multi-carrier, LDPC 0.4/0.6/0.8 and QAM - the receiver detects them itself (existing). The classifier test: PN correlation peaks at the PN length intervals (distinguishes DTMB from DVB-T: DVB-T has CP not PN).
- Tests: `dtmb_gen.h`; multi = N copies at 8 MHz.

### 2.8 DVB-T / T2 (existing)
Moves behind the adapter unchanged. Cost: T 8K 64-QAM 0.51 (all threads, 0.26 feed thread pipelined), T2 32K 256-QAM 0.95 (feed 0.32). Min capture 8 MHz x 1.25 (7 MHz VHF, 6 MHz Asia).
Existing generators and the existing MultiTv tests (tests/engine multitv; look up exact names before phase 1) stay the regression for phase 1.

### 2.9 Analogue TV
- Plug-in: `atv_rx.h`, generator `atv_gen.h`, `atv_testkit.h`, standards in `atv_std.h` (PAL B/G/D/I, NTSC M, SECAM). Terrestrial: 6-8 MHz channel raster, vision AM-VSB carrier + FM/NICAM sound. Satellite analogue: wideband FM (27 MHz, video FM-modulated with energy dispersal, sound subcarriers 6.5 MHz): a free-frequency signal with a non-flat spectrum.
- Cost: unknown, not benchmarked; expected well below 0.3 core per channel (no FEC). Mark TODO bench.
- Min capture: 8 MHz + guard terrestrial; 30 MHz+ for satellite FM TV.
- Specific: no transport stream. Sink = frames (video) + audio. The pool needs a "video sink" for the selected channel and thumbnails for the others
  (tiles, 2.10). The edge detector's flat-top assumption fails for FM (triangular spectrum): classify by vision carrier line (AM TV: strong narrow carrier at 1.25 MHz above lower edge, line-rate sidebands at 15.625 kHz) or for FM TV by the 25 Hz/50 Hz energy dispersal triangle. Analogue shutdown means few real signals; keep last, low priority.
- Tests: `atv_gen.h`; multi = shifted copies including FM-satellite generator if it exists (test_atv_fm.cpp suggests FM exists; confirm).

### 2.10 What the services/EPG/recording/player share
Today `MultiService{slot, offsetHz, TsService}` and `Engine::multiServices()` already give the merged service list. Extend with:
- `MultiService::signalId`, `stdId`, `centerHz` (IF and sky), `bwHz`, `rsHz`, `isi`, `snrDb`, `locked`: the UI shows a "Signal" column and can group/filter by signal.
- Service identity key = (onid, tsid, serviceId) as today; duplicate services seen on two signals (the same programme on two transponders) are shown once, with the better-SNR signal marked as source, the other as fallback.
- Playback of a non-selected signal: `selectTvChannel(slot)` today stops the player and switches the sink (tsSnap_ copied, `player_.select(-1)`). Keep that for "play from signal X" but with a fast path: the target
  signal's `TsDemux` is already running, so only the sink swaps (target: < 200 ms to first picture, limited by the next PAT/PMT/IDR). Add `Engine::selectSignal(SignalId, serviceId)` that also starts the player on the service.
- Recording from non-selected signals: the pool's per-signal `TsDemux` can feed a recorder directly: `Sink` becomes a set of per-signal sinks (`addSink(signalId, fn)`), each recorder/stream/network-tuner client holds one. A recording keeps its signal at priority (2) in the pool. Recording the TS needs only the TS packets; PIDs filter in the demux.
- Picture-in-picture/tiles: decode video only for the selected + up to N preview services (decoder CPU is separate from the receiver CPU; software decode of a second HD stream costs ~0.2-0.5 core; default 1 PIP, "tiles" decoding thumbnails from I-frames only with decimated rate). Audio from the selected service only.
- EPG: already per slot (`epg(slot)`): merge by service key, show the signal in a tooltip. Satellite: EPG from EIT-other/OpenTV is the same TS path.

## 3. UI (app/panels/tv.cpp)
- Multi-signal spectrum: the existing spectrum widget (app/gfx/plots.cpp) gets overlay bands for each `SignalCand`: coloured by state (grey = seen, no receiver; amber = searching; green = locked; white outline = selected), label = standard + Rs/BW; click selects, shift-click toggles decode. Raster standards show the raster ticks as today.
- Signal table (extends `TvChannelRow`; add `signalId, std, centerHz(IF), skyHz, bwHz, rsHz, modcod, isi, services, drops`): columns: play button, frequency (IF and sky for satellite), bandwidth / symbol rate, standard, lock state, SNR, MODCOD/mode, services count, network name, CPU %.
- Per-signal play button -> `Engine::selectSignal`. "Decode all" switch (pool at budget) vs "Selected only" (pool = 1) vs "Choose" (user ticks; manual signals override the budget with a warning).
- CPU meter: bar with total pool CPU vs budget, per-signal in the table; receiver limit control (Auto / 1..8) next to `setMultiChannel`'s existing max channels; a dropped-samples warning (existing `dropped`).
- LNB panel for satellite (LO, band, polarisation, 22 kHz/DiSEqC notes), "next slice" and "scan all slices" buttons.
- Android: the budget defaults to 1-2 receivers on phones (thermal; perf-power-030 hook), the PSD analysis at lower refresh (2 Hz), the signal table is a bottom sheet instead of a side panel, tap on a marker = play, long press = decode toggle. RTL-SDR over USB (driver) is the main radio: 2.4 Msps -> SCPC only. Wide radios on Android are rare.

## 4. Implementation phases

| # | Phase | Size | Notes / tests |
|---|---|---|---|
| 1 | Refactor `MultiTv` into `SpanTv` + `TvAdapter` (DVB-T/T2 adapters only), raster scanner unchanged, `MultiTv` stays as wrapper, `TvChannelRow` keeps its fields. No behaviour change. | M (~1.5 weeks) | existing MultiTv tests identical; add golden: same TS digests (`bench_dvb` digest approach) before/after |
| 2 | `SpanAnalyzer` (free-frequency detection) + `SignalTracker` + pool budget (live CPU) + signal table UI | L (~2 weeks) | unit: synthetic multi-carrier PSD cases (bw/edge accuracy +-5 %), hysteresis, budget with fake adapters |
| 3 | DVB-S/S2/S2X narrow transponder support: adapter, Rs/roll-off estimate, LNB config, inversion try, ISI handling | L (~2-3 weeks) | multi-carrier generator from `dvbs_tx`; test 2 and 3 carriers in 2.4 Msps, 6 carriers in 10 Msps, tilt, adjacent carrier at -10 dB; RTL-SDR 2.4 Msps real-time CPU check on the target |
| 4 | DVB-C (+ optimise baseline 1.13), ISDB-T, ISDB-S (needs branches merged first) | M each (~1 week each) | N-copy generators; ISDB-S needs >= 40 Msps radio or synth source |
| 5 | ATSC 1.0 + ATSC 3.0 + DTMB (raster path, pilot/PN classification) | M (~1 week each) | N-copy generators, pilot-offset case |
| 6 | Blind classifier (`BlindClassifier`) across all adapters, Auto mode | L (~2 weeks) | mixed-standard synthetic span (DVB-T + ATSC + ISDB-T at 6 MHz) must be classified 100 %; false-positive test on noise and on LNB tones |
| 7 | Analogue TV (video sink, tiles), PIP/recording-from-any-signal in engine | M-L (~1.5 weeks) | atv_gen copies; recording test with two signals, one selected |
| 8 | Synthetic wideband source (`synth_wide.h`) as an input of SpanTv; two-stage polyphase channelizer if measured needed | M (~1 week) | `synth-wideband2-030` tests; DVB-S2/NBTV carriers inside one slice are the first expected to work |

Phases 1-3 deliver the owner's priority. Sizes are working-time estimates (S < 3 days, M ~ 1 week, L ~ 2-3 weeks), with Sonnet workers.

Existing test helpers to reuse: `dvbs_gen.h`, `dvbs_tx.h`, `dvbs_testkit.h`, `isdbt_gen.h`, `isdbs_gen.cpp`/`isdbs_synth.cpp`, `atsc_gen.h`,
`atsc3_synth.h`, `dtmb_gen.h`/`dtmb_tx.h`/`dtmb_testkit.h`, `atv_gen.h`/`atv_testkit.h`, the DVB-T/T2 mode test signals (mode_synth.h, synth_loop.h) and
`bench_dvb` (CPU per signal second) / `bench_dsp` (`tv_channel`).

## 5. Risks and open questions
Risks:
- CPU: satellite big transponders and ISDB-S can eat 2-3 cores each; budget logic must never starve the selected signal; phones are the hard limit.
- Channeliser cost at wide spans (N x spanRate mixing): measure; polyphase stage may be needed earlier than planned.
- Radios' dynamic range (8-bit RTL-SDR) with several carriers of different levels: weak carriers lost in quantisation noise; AGC is per radio, not per signal.
- Blind detection false positives (LNB spurs, tones, images, adjacent-carrier shoulders) and long lock times for satellite blind search.
- Two-radio synthetic wideband: seams for wide signals are unmeasured; only signals inside one slice are reliable.
- DVB-C2 and DVB-S2X wideband (WB) modes, ISDB-S3, ATSC 3.0 LDM are not covered by existing receivers.
- Shared `Engine` state (tsSnap_, player_, outputs_) is global to one selected signal; several recorders at once need the sinks refactor in 2.10.
- Decision on whether `WideBank` and `SpanTv` should be unified: this design keeps them separate (pool of threads vs `parallelFor`), sharing only the analyzer/tracker.

Open questions for the owner:
1. Satellite first target hardware: RTL-SDR 2.4 Msps (SCPC only) or a wide radio (Airspy/HackRF/Lime)? This drives the first test signals.
2. Is LNB handling (LO frequencies, voltage, tone, DiSEqC) in scope for the first release, or only manual IF tuning plus a fixed LO setting?
3. Default behaviour: "decode all" within the budget, or only the selected signal until the user switches it on (battery)?
4. Should services duplicated across transponders be merged in the list, or all be listed with the signal column?
5. Recording from a non-selected signal in 0.3.0, or later?
6. Is ATSC 3.0 (ALP/IP) allowed into the TS-based service list, or does it get its own list?
7. Analogue TV: is it worth the video-sink work at all (shutdowns)?
8. Maximum receivers allowed by default on desktop (3 today) and on Android (1?).
