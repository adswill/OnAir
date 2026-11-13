# Which radios can transmit in OnAir

Transmit is only possible through the Arm + licence confirmation + band-plan check, whatever the radio. This table says which radios have a transmit path at all.

| Radio | Transmit in OnAir | How | Should it? |
|---|---|---|---|
| HackRF One / Pro | yes | OnAir's own USB driver (libhackrf fallback has none) | yes |
| PlutoSDR (native, libiio) | yes, also full duplex | AD9361 TX attenuation, 1 output (2 on 2T images) | yes |
| USRP B200/B210/B200mini/B205mini (native, UHD) | yes, since 3.0 | UHD tx_streamer, fc32/sc16, TX/RX connector, B210 outputs RF A and RF B; needs UHD 3.15 or newer with the C API TX functions. The several RX entries of one USRP appear as ONE radio in the TX list. Not full duplex: a USB USRP opens once, so the receiver must stop first | yes |
| LimeSDR (native) | no | native driver is receive-only; transmits through its SoapySDR module (SoapyLMS7) | later: native TX |
| bladeRF (native) | no | same: use SoapyBladeRF | later: native TX |
| SoapySDR radios with a TX channel | yes | Soapy TX stream | yes |
| SDRplay RSP* | no | the hardware is receive-only | no |
| Airspy R2/Mini, Airspy HF+ | no | the hardware is receive-only | no |
| HydraSDR RFOne (native, libhydrasdr) | no | receive-only; three RF inputs (ANT with the bias tee, CABLE1, CABLE2, the radio's "RF input" setting). USB id 1d50:60a1 with firmware before 1.0.2, 38af:0001 since; Windows binds WinUSB by itself | no |
| RTL-SDR (also rtl_tcp) | no | "RTL-SDR dongles can only receive" | no |
| KiwiSDR, SpyServer, RX888, Fobos | no | receive-only | no |
| Hamlib rigs | CAT only | handled by the Hamlib sink | yes |

The TX window shows a radio that cannot transmit as a short line ("2 receive-only radios hidden", with a show toggle); the reason names the radio once, e.g. "USRP B210 000000001 cannot transmit in this build: the installed UHD has no transmit functions".

Not tested on real hardware: the USRP TX path was written against UHD's C API and tested with fakes only (open without UHD, classification, texts). Test with a dummy load and the Arm gate.

## DVB-T / DVB-T2 / DVB-C USB sticks with an RTL2832P (issue 180)

Sticks such as the Astrometa DVB-T2/C (USB id 15f4:0131; the HanfTek DAB+/FM stick is 15f4:0133) carry an RTL2832P and an R828D tuner. OnAir opens them
as an ordinary RTL-SDR (the ids are in the RTL tables of the Linux udev rules, the desktop USB list and the Android device filter), so they show up in the radio
list and work in every mode that takes raw samples; DVB-T and DVB-T2 are decoded by OnAir's own software demodulator. The stick's own hardware demodulator (the
separate demodulator chip of the DVB-T2/C models) is **not** used. On Linux the kernel DVB driver may grab the stick first: blacklist `dvb_usb_rtl28xxu`.
Sticks with another id: report the `lsusb` line. Needs a user test on the DVB-T2/C model.

## SDRplay RSP1 and Mirics MSi2500 sticks: OnAir's own driver (issue 503), untested on hardware

The original SDRplay RSP1 (USB id 1df7:2500, also its clones) is a Mirics MSi2500 USB bridge with an MSi001 tuner. OnAir drives it itself
(`core/src/source/mirics_usb.cpp`), on Android straight over USB (no RSP-TCP, no SDRplay API, which does not exist for Android) and on Linux,
macOS and Windows through libusb. TV sticks with the same two chips are listed too: Hauppauge WinTV 133559 LF (2040:d300), AverMedia A859
(07ca:8591), IO-DATA GV-TV100 (04bb:0537), Logitec LDT-1S310U/J (0511:0037). The RSP1A, RSP1B, RSP2, RSPduo and RSPdx are other chips and
stay with the SDRplay API.

**Untested on hardware.** The driver was written from libmirisdr's register descriptions without an RSP1 on the desk; its register maths and
sample unpacking are unit-tested (`tests/source/test_mirics.cpp`), the radio itself is not. The radio list shows "(untested on hardware)"
until a user confirms it works.

- 10 kHz to 2 GHz, 1.3 to 12.096 Msps. The sample format follows the rate: 14-bit up to 6.048 Msps, 12-bit up to 8.064, 10-bit up to
  9.216, 8-bit up to 12.096 (radio setting "Sample format").
- One gain slider, 0 to 102 dB: IF 0-59 dB, then the mixer (+19 dB) from 19 dB, then the LNA (+24 dB) from 43 dB. "LNA" and "Mixer gain"
  in the radio settings force either on or off.
- IF filter: Auto (the widest of 0.2, 0.3, 0.6, 1.536, 5, 6, 7, 8 MHz that is not wider than the rate) or one width.
- "Front end": SDRplay RSP1 (default for 1df7:2500) or Generic stick: the band-switch words differ. Try the other one if a band is deaf.
- "USB transfer": isochronous (default on Linux, macOS, Android) or bulk (default on Windows; try it on a phone that drops samples).
- Desktop: when the SDRplay API lists the RSP1, its entry is used and this driver's entry is hidden (the API's service holds the radio).
  Linux: the kernel's msi2500 module is detached when the radio is opened. Windows: the radio needs the WinUSB driver (Zadig) for this driver.
- Android: needs an APK built with `LIBUSB_SRC` (android/docs/USB.md), like the HackRF.

## HackRF baseband filter
HackRF One / Pro: Advanced > Baseband filter. Auto (default) follows the sample rate (about half of it, wider for a wide channel, the mode's own narrow filter when running at the mode's rate); or pick one of 1.75, 2.5, 3.5, 5, 5.5, 6, 7, 8, 9, 10, 12, 14, 15, 20, 24, 28 MHz. The choice applies at once and is not limited by the sample rate.
