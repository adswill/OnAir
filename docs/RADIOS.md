# Supported radios

Per-radio test results and limits: [DEVICES.md](../DEVICES.md). Network radios in detail: [NETWORK_RADIOS.md](NETWORK_RADIOS.md).

OnAir supports the **HackRF One and HackRF Pro** natively, and drives these radios directly too:

| Radio | Wide TV channels (6 – 8 MHz) | DAB, FM and narrow channels | Notes |
|---|:---:|:---:|---|
| **HackRF One / Pro** | ✅ | ✅ | The radio OnAir is developed and tested with |
| **BladeRF**, **LimeSDR** (USB, **Mini**, Mini 2) | ✅ | ✅ | LimeSDR-USB: external reference clock (10-40 MHz on CLK IN) in the radio settings; any SoapySDR radio that lists clock or time sources gets the same choice. LimeSDR Mini: 10 MHz - 3.5 GHz, up to 30.72 Msps (USB 3 above about 15), listed by name; Windows needs FTDI's FT60x driver, and `LimeUtil --update` if the gateware is old. See [DEVICES.md](../DEVICES.md) |
| **USRP** | ✅ | ✅ | Reference clock (internal, external 10 MHz, GPSDO) and time source (PPS) are picked in the radio settings. On Windows, install UHD from [Ettus](https://files.ettus.com) (with its firmware images) |
| **PlutoSDR** | ⚠️ | ✅ | Over its USB cable it streams about 4 Msps, too little for a 6 – 8 MHz channel. Library not included on macOS yet: install libiio 0.26 (Analog Devices' macOS package, or Homebrew) |
| **Airspy R2 / Mini**, **SDRplay RSP** | Should work | ✅ | SDRplay: install the [Hardware API](https://sdrplay.com/hardware-api/) first (RSP1, RSP1A, RSP1B, RSP2, RSPdx, RSPdx-R2). An Airspy Mini lists 6 and 3 Msps; 10 Msps is an opt-in in its settings, untested on a Mini |
| **HydraSDR RFOne** | Untested | ✅ | Native driver, receive only, 24 MHz to 1.8 GHz, 10 Msps. Needs `libhydrasdr` installed (loaded at run time, not included), see [DEVICES.md](../DEVICES.md) |
| **Aaronia SPECTRAN V6 / V6 ECO** | Untested | Untested | Receive and transmit through Aaronia's RTSA API; written from Aaronia's documentation, **needs a user test**. Install the RTSA-Suite PRO (Windows or Linux, not included), see [docs/SPECTRAN-V6.md](SPECTRAN-V6.md) |
| **SDRplay RSPduo** | Streaming tested at 10 MS/s; TV decoding untested | FM and RDS tested on Windows | Select Tuner 1 SMA, Tuner 2 SMA, or Tuner 1 Hi-Z; one input at a time. [Setup and validation](../DEVICES.md#sdrplay-rspduo) |
| **RTL-SDR** | ❌ | ✅ | An RTL-SDR Blog V4 or V4 Lite works for HF on macOS too |
| **Airspy HF+** | ❌ | Narrow channels only | Specified for HF and 60 – 260 MHz |
| **Fobos SDR** (agile firmware) | ✅ | ✅ | Contributed, untested here. Specified for 50 MHz – 6 GHz on the RF input, HF on the HF1 / HF2 inputs. Library not included: put RigExpert's `fobos_sdr` library next to OnAir, see [DEVICES.md](../DEVICES.md) |
| **RX888 MkII** | ✅ | ✅ | Native SDDC/USB driver (FX3 firmware, 16-bit ADC), still being hardened on real hardware. USB 3 required. Packages include `SDDC_FX3.img` (RAM load only); see [DEVICES.md](../DEVICES.md) |
| **Icom IC-R8600 (I/Q OUT)** | ✅ | ✅ | Native USB driver, receive only, 10 kHz to 3 GHz, 0.24 to 5.12 Msps. Needs Icom's own firmware file (imported once, never shipped); **needs a user test**. See [DEVICES.md](../DEVICES.md#icom-ic-r8600-iq-out-receive-only) |
| **RFNM** | ✅ | ✅ | Contributed. Only in builds made with [librfnm](https://github.com/rfnm/librfnm) installed (not the downloads yet), see [DEVICES.md](../DEVICES.md). One entry per receive channel of the daughterboards |
| **ESP32 / nRF24L01 module** (USB serial) | ❌ | ❌ | nRF24 (2.4 GHz), BLE adverts and WiFi observation through your own ESP32 and the firmware in `firmware/esp32/`; WiFi is observed only, never transmitted. **Untested on real hardware**, see [docs/ESP32-NRF24-DESIGN.md](ESP32-NRF24-DESIGN.md) |
| **Anything else** | depends | depends | Through [SoapySDR](https://github.com/pothosware/SoapySDR), once its SoapySDR module is installed |

- The libraries these radios need are included in the downloads, except as noted above.
- A TV channel needs a radio that samples fast enough: roughly 1 million samples per second per MHz of channel width. If a radio cannot reach the rate or frequency a mode needs, OnAir says so next to the source.
- **Any frequency.** OnAir never limits the frequency you enter, in any mode and on any radio. The ranges listed for a radio are what it is *specified* for: outside them OnAir still asks the driver and shows a note. An RTL-SDR picks the way down below 24 MHz by itself (an RTL-SDR Blog V4 upconverts HF in its library, any other dongle samples its Q input directly). Only what a driver itself refuses stays an error.
- The radios other than the HackRF have had less real-world testing. Notes for specific radios are in **[DEVICES.md](../DEVICES.md)**.

<details>
<summary><b>Sound card and Hamlib rig sources</b></summary>

- **Sound card** (the radio list: **Sound card: …**, including Default input): WebSDR, a virtual audio cable, or a handheld radio's speaker/line into the computer, for HF decoders (FT8, RTTY, CW, SSB) at the capture rate (usually 48 kHz). Four mappings: I/Q stereo, USB audio, LSB audio, mono real. The frequency field is the *dial* the decoder should assume; nothing is tuned. Unverified against a real WebSDR.
- **Hamlib rig** (source list: **Add Hamlib rig…**): a CAT-controlled radio such as an Icom IC-R8600 or IC-7300. Search the model by name or type its Hamlib number, pick the serial port OnAir finds (or `host:port` for a `rigctld` on the network), and choose a sound card for the audio or IQ. The frequency and mode follow in both directions. If the rig is unplugged or `rigctld` stops, OnAir says so and reconnects by itself. Receiving never keys the transmitter. **Rotators** are driven the same way.
</details>

### Network radios

Add them from the source list: **Add network radio…**, choose the type, type its address and press **Connect**. OnAir tries for a few seconds and says why when it gets no answer; added radios are remembered.

<details>
<summary><b>Network radio types, ports and notes</b></summary>

| Type | Address | Port |
|---|---|---|
| **PlutoSDR** by IP (USB network or LAN, with a Link setting Auto / USB / Ethernet on a Pluto+; needs libiio 0.23 – 0.26 installed) | `192.168.2.1`, or its address on your network | libiio's default |
| **rtl_tcp** (an RTL-SDR shared by `rtl_tcp -a 0.0.0.0`) | the computer running `rtl_tcp` | 1234 |
| **SoapyRemote** (any SoapySDR radio shared by `SoapySDRServer --bind`) | the computer running the server | 55132 |
| **SpyServer** (Airspy's server, as SDR# and SDR++ use it) | the computer running SpyServer | 5555 |
| **SDR++ server** (SDR++ run as `sdrpp --server`, any source SDR++ has) | the computer running it | 5259 |
| **KiwiSDR** (HF receiver on the network, 12 kHz IQ; experimental, see [docs/kiwisdr.md](kiwisdr.md)) | the receiver (or **Find KiwiSDR** for the public list) | 8073 |
| **SDRplay nRSP-ST** (through SDRconnect's WebSocket API; experimental, see [docs/nrsp-st.md](nrsp-st.md)) | the computer running SDRconnect, with its WebSocket server on | 5454 |
| **TCI (network)** (Expert Electronics' Transceiver Control Interface of ExpertSDR3, SunSDR and other programs with a TCI server; receive only, see [docs/NETWORK_RADIOS.md](NETWORK_RADIOS.md)) | the computer running the TCI server | 50001 |
| **Hermes Lite 2** | found by itself on your networks (or type its address) | 1024 |
| **Red Pitaya** (SDR transceiver app, HPSDR protocol 1) | found by itself on your networks (or type its address) | 1024 |

- rtl_tcp carries 8-bit samples at up to about 2.5 Msps (more with the Auto server radio setting). SoapyRemote needs SoapySDR with the SoapyRemote module on both computers; if it picks the wrong radio or lists none, type the radio's driver (for example `mirisdr`) in "Remote driver". Do not confuse SpyServer's port 5555 with SoapyRemote's 55132.
- **SpyServer** is spoken from the public protocol description (2.0.1700): the full IQ stream (8-bit or 16-bit), sample rates from the server's decimation stages, frequency and gain control. Tell us if your server misbehaves.
- **SDR++ server**: OnAir sets the sample rate and the gain through the server's own source menu, so it works with whatever radio SDR++ drives there, and that source's other controls (gain mode, bias tee, AGC, antenna, the server's source) show up under **Radio settings** as "SDR++: ...". Those are changes to the server: SDR++ keeps them after OnAir disconnects. The server takes one client at a time. See [DEVICES.md](../DEVICES.md).
- **Hermes Lite 2** receives up to 38.4 MHz at 48 to 384 ksps, so only HF and narrow modes fit; the N2ADR filter board's band filters follow the frequency. **Red Pitaya** runs Pavel Demin's HPSDR-compatible SDR transceiver app: 48 to 384 ksps, tuning up to 61.44 MHz, no gain control; tested only against a fake server, so sample rate accuracy, level scaling and mirroring (setting "Swap I and Q") are unverified.
- **Several receivers (slices) of one Hermes Lite 2 or Red Pitaya**: the radio list has one entry per receiver ("Hermes Lite 2 RX1 ...", "Hermes Lite 2 RX2 ..."; as many as the HL2's gateware says it has, three on a Red Pitaya), so each OnAir receiver tab can take its own, on its own frequency and mode. They share the radio's one stream, so they share its sample rate (the first one opened sets it), its gain and the filter board (the filter for the highest frequency in use). Opening or closing a receiver above the others restarts the stream for a fraction of a second. One program at a time can stream from the radio.
- When a network radio drops, OnAir reconnects by itself, and the status bar shows the samples lost. Nothing is opened to the network by default.
- **Share my radio** (View > Data feeds): OnAir becomes an rtl_tcp server for the radio it is running, so a second OnAir, SDR#, SDR++ or any rtl_tcp client can listen to the same signal (8-bit IQ at the radio's rate). Only this computer can connect until you tick **Let other computers connect (LAN)**; clients can change the frequency and gain only when **Allow remote control** is ticked.
- **Data feeds** (View > Data feeds), all off until you turn one on: ADS-B SBS-1 and Beast (TCP), AIS NMEA (UDP), APRS-IS and KISS (TCP), ACARS and VDL2 JSON (UDP), GNSS NMEA RMC/GGA (UDP), Bluetooth LE JSON (UDP), LoRa / LoRaWAN / Meshtastic JSON (UDP). The TCP servers stay on this computer unless **Let other computers connect (LAN)** is ticked.
</details>
