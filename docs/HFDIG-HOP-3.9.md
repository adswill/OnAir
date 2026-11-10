# Band hopping for FT8 / FT4 / FT2 / WSPR (3.9)

Round-4 item 17 (scratchpad r4-per-mode list; WSJT-X Band Hopping). Branch feat39-r4.

- **FT8 / WSPR tab > Band hopping**: with **Hop bands every 2 minutes** on, the dial moves at the start of each 2-minute UTC slot to the band for that slot. It uses the dial frequency of the entry's mode (WSPR, FT8, FT4 or FT2) from the multiband table, which holds the WSJT-X defaults. The radio follows when OnAir controls it.
- The plan is a table of UTC hours by bands (160 m to 6 m), with **all** and **copy down** per row. The current hour is highlighted. It is saved (`hfdigHopPlan`).
- **Coordinated** (on by default): the 20-minute cycle 160, 80, 60, 40, 30, 20, 17, 15, 12, 10 m, one band per 2-minute slot counted from the hour. That way every station using it is on the same band in the same slot. A band switched off for that hour gives its slot to the next band in the cycle that is on. 6 m is used only when no band of the cycle is on.
- Not coordinated: the bands that are on for the hour take turns, one per slot.
- Hopping is hidden while whole-span multiband decoding is active, because that covers several bands at once.
- **From memory**: the order of the coordinated cycle is WSJT-X's as far as known here (160 m at minute 0, then one band up every 2 minutes). It was not checked against the WSJT-X source.

## Tests
`tests/hfdig/test_hfdig_hop.cpp`: the coordinated cycle over 40 minutes, the slot edges and the next change, a band switched off, 6 m alone, the plain rotation (also for negative slots), different bands per hour and an hour with none, and the settings text. Run locally: all passed. The retune was syntax-checked only.
