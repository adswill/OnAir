# OnAir 3.6

New modes, a lot of work on how well the decoders hear weak signals, and an easier way in.

**New modes** (178 in all)
- GK-2A LRIT and HRIT, FengYun-4 LRIT / HRIT, FY-2 S-VISSR, FY-3 AHRPT and MPT.
- PACTOR-II/III/IV (physical layer), SITOR-B, SELCAL with a call log, selective calls in the pager modes.
- Inmarsat Aero C voice and Iridium voice (codecs are installed by you).
- eLoran, DME, DGPS, Yaesu Fusion, MPT1327, EDACS and LTR are no longer labelled work in progress.

**Easier**
- Simple / Advanced view per mode, Scan and Identify, first-run setup with regional presets.

**Better decoding**
- Sensitivity measured against theory; ATSC 3.0, DVB-S2, P25, HFDL and others improved.
- Auto gain rebuilt; digital voice modes explain why they are not decoding.

**More ways to use it**
- Everything in view: several signals from one wide capture.
- Multi-radio trunking, voice watch list, pager keyword alerts.
- Share my radio through a relay, a redesigned OnAir Web, and an MCP server for AI assistants.
- Record and replay in any mode; export to CSV, KML and GeoJSON; custom bandwidth and sample rate in every mode.
- Linux AppImage with in-place update.

Full list: docs/CHANGELOG-3.6.md. Measured numbers come from OnAir's own test signals, not over-the-air recordings, unless stated.
