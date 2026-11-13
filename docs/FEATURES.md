# Features

**Watch and listen**
- **Live player** with picture, sound, subtitles, teletext, programme guide and multiple audio tracks; record a service to a file, pause and live buffer.
- **Automatic repair** of short signal dropouts, so weak reception stays watchable.

**See your signal**
- Spectrum and waterfall (colour maps, pause and scroll-back, averaging, max hold, peak markers), every constellation, signal-to-noise and error figures.
- A single **quality score**, and an **echo (multipath) detector**.
- **Neural noise remover** for speech in the General receiver (NFM, WFM, AM) and Airband: RNNoise ([BSD-3-Clause](../third_party/rnnoise/COPYING)) in the Listen chain, off by default, with a strength slider. See [docs/noise-remover.md](noise-remover.md).
- **DAB transmitter map:** OnAir reads the transmitter identification (TII) that DAB networks send and lists the transmitters heard.

**Tablets and touch screens**
- **Touch interface** (View side panel > Touch interface, or `--touch`): 48 px targets, a bottom dock with Start / Stop, tuning, gain, volume and tuner, a numeric keypad for the frequency (MHz, kHz, GHz), a full-screen mode picker in the mode list's colours, bottom-sheet dialogs, drag and fling scrolling, swipe between tabs, long press for the context menu, portrait and landscape.

**Get the best reception**
- **Memories:** keep the frequencies you use with their mode, sample rate, gains and the mode's options (`Ctrl+M`, or the **M** menu): search, favourites, groups, one-click recall on any receiver, import and export as CSV / JSON (also plain `frequency,name` lists and SDR++ bookmarks), and a scan of the memories you flag.
- A **channel scanner**, **automatic gain tuning**, a **gain optimiser**, and a **direction finder** that helps you aim the antenna; a **pass planner** for satellites.
- **Your radio, set up properly:** every antenna input is its own entry in the source list, with per-radio settings.
- **Up-converters and LNBs:** Ham It Up, Ham It Down (1500 MHz), SpyVerter, QO-100 / Bullseye TCXO / Ku LNB or a custom LO.
- **DC spike handling:** leave the radio's centre spike alone, remove it, or keep it off the channel by tuning.

**Share it**
- **[map.on-air.dev](https://map.on-air.dev)** is the public live map of aircraft, ships, APRS, mesh nodes and more (asked for at start, off until you agree).
- A built-in **network tuner** streams your channels to VLC, phones, Plex or Jellyfin over your home network. **Server > Share this radio** puts your radio in a browser page with one button (link and QR code, see [docs/ONAIR-WEB.md](ONAIR-WEB.md)), and **Share my radio** in Data feeds serves it as rtl_tcp.
- On a Mac, **cast** the playing service to an Apple TV or AirPlay TV. **Record** to a file, or send the stream out over UDP.

**Comfortable to use**
- **Scroll to tune** and the **mouse wheel on any slider** (volume, gain, squelch, bandwidth).
- **Logbook export:** "Save results..." writes CSV or JSON, an ADIF logbook and more.
- **Light**, **High contrast** and dark themes with your own accent colour; the layout adapts to small windows and display scaling.
- **Fast:** error correction runs on the GPU where available, and with optimised vector code on the CPU everywhere.
