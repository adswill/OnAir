# OnAir 3.7

More satellite modes, one-button sharing, an Android app that lists every mode, a reworked auto gain, and a lot of smarter decoding. Most of it was tested on generated signals, so please read the known limits below.

**New modes** (191 in all)
- NOAA APT, HRPT, GAC and DSB, Meteor HRPT, Arktika-M RDAS and GGAK (experimental), Elektro-L RDAS, AWS.
- Inmarsat BGAN (signalling only), Sub-GHz sensors, Sferics (lightning), Thread, Zigbee on 868 / 915 MHz.

**Share my radio**
- Server > Share this radio: one button, a link and a QR code, no router settings.

**Android**
- The mode list has every desktop mode (183 rows, was 108); a receiver freeze and wrong panels fixed.

**Better**
- Auto gain reworked for every radio type.
- FM and AM censuses, FT8 cross-slot combining, RTTY auto lock, pager text repair, soft-decision voice frame correction, mesh topology, CPDLC messages, Aero acquisition about 1 dB better.
- Auto setup and Reset this mode in every mode panel.
- Fixes for RTL-SDR, Pluto, SDR++ and SpyServer radios; thanks to peterdewitte, bellx2, DME-3 and BI8SYN for their pull requests.

**Known limits**
- Most new decoders were tested on generated signals. Real recordings were used for NOAA APT, NOAA HRPT and Meteor HRPT only. BGAN has not been seen on a real signal.
- Auto gain was simulated only; 5 of 40 test cells still miss a target (HackRF FM, SpyServer FM, HackRF wide, Airspy wide, Airspy ADS-B to wide).
- Android was tested on an emulator only; wide TV modes will not keep up on a typical phone, and there is no TV picture, voice or satellite picture view yet.
- Sharing uses temporary Cloudflare links until the OnAir relay is hosted. Arktika-M GGAK is experimental. DRM issues #331, #332 and #423 are not fixed.

Full list: docs/CHANGELOG-3.7.md. Measured numbers come from OnAir's own test signals unless stated.
