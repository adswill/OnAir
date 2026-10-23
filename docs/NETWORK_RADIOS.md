# Network radios

A radio on another computer, a Raspberry Pi in the loft, or a receiver on the internet: OnAir takes its IQ samples over the network and
decodes them like a radio on USB. Add one with **Radio list > Add network radio…** (also on the first-run screen), pick the type, type the
address, press **Connect**. It is then in the Radio list with the others and is remembered.

## Which one to pick

| Server on the radio's side | Type in OnAir | Default port | Good for |
|---|---|---|---|
| `rtl_tcp` (any RTL-SDR) | rtl_tcp | 1234 | an RTL-SDR on a Pi; simplest, 8-bit, up to 2.4-3.2 Msps on a LAN |
| SDR# / Airspy **SpyServer** | SpyServer | 5555 | Airspy and RTL-SDR, also over the internet (it sends only the part you watch) |
| **SoapySDRServer** (SoapyRemote) | SoapyRemote | 55132 | any radio with a SoapySDR module (SDRplay, LimeSDR, HackRF, USRP ...) |
| **SDR++** started as `sdrpp --server` | SDR++ server | 5259 | whatever SDR++ supports, one client at a time |
| **KiwiSDR** (local or public) | KiwiSDR | 8073 | HF 0-30 MHz, 12 kHz of IQ; **Find KiwiSDR…** lists public ones |
| **ExpertSDR3 TCI** (SunSDR) | TCI (network) | 50001 | SunSDR radios, receive only |
| **SDRconnect** WebSocket server | SDRplay nRSP-ST (SDRconnect) | 5454 | the network RSP |
| PlutoSDR | PlutoSDR (by IP) | libiio | `192.168.2.1` over USB gadget, or its Ethernet address |
| Hermes Lite 2 / Red Pitaya | Hermes Lite 2 / Red Pitaya | 1024 (UDP) | found by itself on the LAN (Search) |
| anything else SoapySDR knows | SoapySDR (device string) | | a raw device string, e.g. `driver=uhd,addr=10.0.0.55` |

OnAir can also be the server: **Share my radio** sends the samples of the radio it runs as an rtl_tcp stream, so a second OnAir (or SDR++,
SDR#, GQRX) can use them.

## rtl_tcp

On the radio's computer: `rtl_tcp -a 0.0.0.0 -p 1234` (`-a 0.0.0.0` listens on every network, the default listens only on the computer
itself). In OnAir: type **rtl_tcp**, the address, port 1234. The sample rate is the one OnAir asks for (keep it at 2.4 Msps or below over Wi-Fi).
rtl_tcp has no way to tell the client which dongle it is, so the RTL-SDR Blog V4's HF upconverter and the V3's direct sampling depend on
the rtl_tcp build on the server (the rtl-sdr-blog one switches them). The status bar counts samples lost on the network.

## SpyServer

On the radio's computer: edit `spyserver.config` (`bind_host = 0.0.0.0`, `bind_port = 5555`, `device_type = AirspyOne` or `RTLSDR`), run
`spyserver`. In OnAir: type **SpyServer**, the address, port 5555. SpyServer chooses the rate from its own list (decimations of the radio's
rate): OnAir picks the smallest one at or above what the mode needs. Gain is the server's (OnAir's slider sets it when the server allows it).

## SDR++ server

On the radio's computer: `sdrpp --server` (port 5259), choose the radio in its own settings. Only one client at a time: close SDR++'s own
window first. In OnAir: type **SDR++ server**, the address, port 5259.

## KiwiSDR

Type **KiwiSDR**, the receiver's address and port (8073), a password if the owner set one, or **Find KiwiSDR…** to pick a public
receiver. OnAir takes the 12 kHz IQ stream: HF modes only (DRM, FT8, WSPR, HFDL, weather fax, time signals ...). Be kind to public
receivers: each one has only a few channels.

## Troubleshooting

- **Cannot connect**: the server is not running, the address is wrong, or a firewall blocks the port. Try from the OnAir computer with
  `nc -vz SERVER PORT` (macOS/Linux) or `Test-NetConnection SERVER -Port PORT` (Windows PowerShell).
- **Samples lost** (status bar): the network is too slow for the rate. Lower the rate (RATE box), use a cable instead of Wi-Fi, or a
  server that sends less (SpyServer, KiwiSDR).
- **OnAir hangs at start-up** with a network radio: see the next section.
- Network discovery is off by default (below): add the radio by its address.

# Network radios: where they are saved, and recovering from a hang

Radios added by address (SoapyRemote, rtl_tcp, SpyServer, KiwiSDR, Pluto, ...) are saved in the settings file, in the line `netRadios=...`
(entries separated by `;`). The file is `settings.conf` in:

- macOS and Linux: `$XDG_CONFIG_HOME/onair/` or `~/.config/onair/`
- Windows: `%APPDATA%\OnAir\`

If OnAir hangs at start-up because of a saved network radio:

1. Start it once with `--safe-start`: no radio is chosen or opened by itself.
2. Close OnAir, open `settings.conf` in a text editor, delete the entry you do not want from the `netRadios=` line (or the whole line), save.

Looking up a SoapyRemote radio runs in the background (it gives up after a few seconds); press the rescan button to see its radios once the answer is in.

## SoapyRemote (a radio on another computer)

1. On the computer with the radio: install SoapySDR, the radio's SoapySDR module and SoapyRemote, then run
   `SoapySDRServer --bind="0.0.0.0:55132"`. Check it there first: `SoapySDRUtil --find` must list the radio.
2. Open TCP port 55132 in that computer's firewall (and UDP, see 4).
3. In OnAir: **Add network radio**, type **SoapyRemote**, the server's address and port 55132. Leave **Remote driver** empty to list what the
   server offers, or name one (`sdrplay`, `miri`, `rtlsdr`, ... or `driver=sdrplay,serial=...`). A server that also has SoapyAudio lists the sound
   card first: pick the radio, not "audio".
4. The samples travel over UDP by default. On a LAN that is the fastest; through the internet, a VPN or a NAT router UDP often does not get
   through: add `prot=tcp` to the Remote driver field (`driver=sdrplay,prot=tcp`; OnAir passes it as SoapyRemote's `remote:prot`).
5. Test from the OnAir computer with `SoapySDRUtil --probe="driver=remote,remote=tcp://SERVER:55132"`. If that fails too, the problem is between
   the two computers (firewall, versions), not in OnAir. Client and server should run the same SoapyRemote version.

## Network discovery is off by default

When OnAir builds the radio list it looks only for radios on USB (and PCIe), plus the network radios you added by address. It does not search
the network by itself: SoapyRemote's server search, SoapyPlutoSDR's mDNS / `pluto.local` lookup, UHD's broadcast for network USRPs, librfnm's
network search and the other SoapySDR modules that look on the LAN are not run (issue #215: a Pluto on somebody's network must not be contacted
just because OnAir started). To have them back, open the Radio list and tick **Search the network for radios**. Adding a radio by address
never needs it.

## TCI (network), receive only

TCI is the Transceiver Control Interface of Expert Electronics' ExpertSDR3 (SunSDR radios); other programs offer a TCI server too. Add it in
**Network radios** with type **TCI (network)**: the address of the computer running the TCI server and port 50001 (the default), after turning the
server's TCI option on. OnAir connects with a WebSocket (`ws://host:50001`), reads the server's start-up commands (device, protocol, `vfo_limits`,
`trx_count`, ... `ready`), then sets the IQ sample rate (48, 96, 192 or 384 kHz: the smallest at or above what the mode needs), puts the receiver's
DDS (panorama centre) and VFO on the tuned frequency and starts the IQ stream (`iq_start`) of one receiver (the **Receiver** setting appears when the
server has more than one). Float32, 24-bit and 16-bit samples are all understood. The frequency range comes from `vfo_limits`. TCI has no RF gain for
the receiver, so the gain slider does nothing here; the server's AGC stays as it is.

- OnAir never transmits through TCI: it sends only `iq_samplerate`, `dds`, `vfo`, `iq_start` and `iq_stop`, and no audio or CW command.
- When the connection drops, OnAir reconnects by itself and counts the samples lost.
- The TCI server is part of ExpertSDR3: it runs only while ExpertSDR3 runs (with **Options > TCI** enabled); a SunSDR on its own does not
  answer TCI. When OnAir says "cannot connect", check the port in ExpertSDR3's TCI settings and that its firewall rule allows it.
- Tested only against a fake server that follows the TCI 1.9 specification. Unverified without a real ExpertSDR3: the URL path, whether the server
  accepts the VFO being moved with the DDS, and the exact meaning of `length` in the block header (the payload size is used).
