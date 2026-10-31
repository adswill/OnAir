# HFDL: follow the best band

HF carries a band only at some times of day, so an HFDL receiver parked on one band is often deaf. In the HFDL tuner (More > Tuner), "Follow the best band" lets OnAir hop for you. It is off by default because it retunes the radio by itself.

What it does (one look per band, no settings):

* Stays on a band that gives at least 4 good PDUs a minute.
* On a silent band it waits about 50 s (a ground station sends a squitter every 32 s), on a band with some traffic about 2 minutes, then moves to the best candidate: the band's recent PDU rate (forgotten over about 15 minutes), a guess for a band not looked at yet, plus a bonus for each frequency that the heard ground stations announce as active in the system table (their squitters list the frequencies in use).
* When everything is silent it visits the eight bands in turn, the one looked at longest ago first.
* A band with some traffic is left only for a clearly better one (1 PDU/min margin), so it does not flap.
* Rare frequencies of a few stations (3.455, 3.9, 4.67, 15.03 MHz) are tried only when announced.
* Off while Whole span is on (that decodes every band the capture holds) and while the second mode is being tuned. Tuning by hand stays possible: the hopper starts a fresh look at the band you chose.

Code: `core/include/dect2/hfdl/hfdl_hop.h` (pure logic), `app/hfdl/hfdl_ui.cpp` (hopTick), test `tests/hfdl/test_hfdl_hop.cpp`.
Untested on real HF: the rates and dwell times come from the squitter timing and a Poisson model, not from recorded multi-band propagation.
