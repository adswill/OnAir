# Wide multi-signal decode

One wide capture, several different signals decoded at once, found automatically (owner pick #9 of 3.5).

Built on what exists: `CarrierFinder` (carrier search), `WideModes` (one decoder bank per mode), `MultiTv` (DVB-T/T2 slots), `VfoBank` (Multi-VFO, AM/NFM) and `autodet` (the identifier). `WideSession` (`core/include/dect2/engine/wide_session.h`) joins them.

## What it does
1. TV: `SpanScanner` finds 8 or 7 MHz slots with a signal; `detectTv` says DVB-T or DVB-T2 (other standards are listed as "identified, no decoder here"). One `MultiTv` per standard runs only the slots of its standard.
2. Narrowband: `CarrierFinder` on the rest of the span (TV zones excluded). Each new carrier is watched about 1.2 s and classified (DMR, P25, NXDN, dPMR, D-STAR, Fusion, M17, TETRA, analogue FM). A carrier the measurement cannot name gets the kind of the known channel it sits on (user list, built-in frequency database: pagers, APRS), else AM on the air band (118-137 MHz, needs the capture's centre frequency), else it stays "unidentified" and can be given a kind by hand.
3. Each carrier goes to its decoder: the `WideModes` bank of that mode (follow list), or the Multi-VFO bank for AM/NFM, whose mixed sound goes to `setAudioSink`.

## The list
`entries()`: kind, what the identifier said, state (identifying, running, off, dropped for CPU, no decoder, unidentified), offset and absolute frequency, level, SNR, locked, cost, info. Control: `setEnabled`, `setPinned`, `setPriority`, `setKind`, `setListen`.

## CPU guard
All decoders' work (analysis thread plus TV receiver threads) is measured against real time. Above the budget (default 0.8, 0 = none) the lowest ranked entry that is not pinned or listened to is dropped, one per 1.5 s, with the reason shown. It comes back when the load has stayed under half the budget for 5 s and the predicted load fits. Entries are dropped between blocks, and the sound path of the entries that stay is not touched.

## Limits
Narrow carriers sit on a 12.5 kHz search raster; one TV channel width per capture (8 or 7 MHz); DVB-S, ISDB-T, ATSC and others are not decoded here. Pagers/APRS are only found on known channels. Tested on synthetic IQ only, not on real captures.
