# Spectrum ghost

Compares the live spectrum with an earlier one. Open the spectrum's menu (the Trace section) and tick **Ghost: compare with an earlier spectrum**:
the earlier spectrum is drawn faintly in violet behind the live line, and signals that are **new** since then are shaded orange.

**Save snapshot now** stores the current averaged spectrum. While the receiver runs, OnAir also keeps a rolling set by itself (every 6 hours,
the newest 28). Snapshots are small files (`ghost-<time>.oag`, up to 4096 bins) in `<data folder>/ghost`; they never contain IQ.

**Advanced**: Compare with (Yesterday, Last week, Latest snapshot, or one picked from the list), Highlight new signals, New above (dB, default 6),
Auto snapshot every (hours, 0 = off), Keep the newest. "Yesterday" and "Last week" use the snapshot nearest in time that covers at least half of
the view; nothing is drawn when there is none. Only the part of the view the snapshot covers is compared, so a different centre frequency or
sample rate is fine. The comparison is of dB per Hz levels; a different gain between the two captures shifts the whole line, so keep the gain when
you want a meaningful comparison (a new signal must rise more than the threshold above the ghost over at least 2 neighbouring pixels).

Code: `app/state/spec_ghost.cpp` (pure: snapshots, columns, new-signal detection, store), `app/gfx/spec_ghost_ui.cpp`. Test: `spec_ghost`.
The Spectrum analyser's band survey (docs/band-survey.md) compares whole sweeps; this is the same idea on the main spectrum.
