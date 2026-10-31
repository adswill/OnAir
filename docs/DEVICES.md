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

## HackRF baseband filter
HackRF One / Pro: Advanced > Baseband filter. Auto (default) follows the sample rate (about half of it, wider for a wide channel, the mode's own narrow filter when running at the mode's rate); or pick one of 1.75, 2.5, 3.5, 5, 5.5, 6, 7, 8, 9, 10, 12, 14, 15, 20, 24, 28 MHz. The choice applies at once and is not limited by the sample rate.
