# EchoLink client (3.9)

Tools > EchoLink... opens the window. It is a network client, not a radio mode, so it has no mode number and no mode-list entry.

**Validation status: mock-tested only.** Every test talks to a mock directory server or to a second OnAir client on 127.0.0.1. Nothing here has contacted the real EchoLink servers or a real EchoLink station. The first real log-in is the user's.

## What it does

- **Log in.** Uses the callsign, password, name and location from the window. The one button "Log in and list stations" opens the UDP ports, logs in and fetches the list. While logged in, the log-in is repeated every 5 minutes, as SvxLink does. Closing the program or pressing Log out sends the OFF status.
- **Station list and search.** Shows the list with search by callsign, description or node number, and a filter for stations, links (-L), repeaters (-R) and conferences (`*NAME*`). Double-click a row, or select it and press Connect.
- **Connect.** Connects to a station, link, repeater or conference. Advanced has "Connect to this address" for a raw IPv4 address. Incoming calls are accepted unless "Accept calls from other stations" is off. A second caller during a QSO gets a BYE.
- **Audio.** GSM 06.10 full rate both ways, 4 frames (80 ms) per RTP packet. Transmit from the microphone uses "Hold to talk", "Talk (latched)" or VOX (threshold and hold time under Advanced). Received audio plays on the speakers with a volume slider. The microphone opens only while connected.
- **Chat and info.** Sends and receives text chat. Shows the station's info text, which for a conference lists who is connected.
- **RF link** ("Link to RF channel"). Off by default and off again at every start. The first time it is turned on, a note about licences, unattended-operation permits, identification and -L/-R registration must be acknowledged. It links a radio on a sound card. The radio's receive goes over EchoLink when it rises above the squelch level. EchoLink audio is played into the radio's input and keys the radio, either through the radio's own VOX or through Hamlib (CAT, RTS or DTR). It works half duplex: the side that started keeps the channel. There is a tail time and a transmit time-out (default 180 s). After a time-out the radio waits until the EchoLink side is quiet. The bridge rules are unit-tested. Using it with a real radio is not tested.

## Credentials

The password is stored in the settings under `echolinkPassword`. `isSecretPrefKey` treats that key as a credential, so a backup keeps it only inside the passphrase-sealed block, the same as the APRS-IS passcode.

The password field never shows the saved value. The password is sent only inside the log-in command to the directory servers named in Advanced (default `servers.echolink.org`). Log lines use `loginCommandRedacted` and show `****`. The tests check that no log line or status text contains the password.

## Protocol

The protocol follows SvxLink's echolib (GPL-2.0-or-later, Tobias Blomberg SM0SVX). It is credited in THIRD_PARTY.md and in the in-app Credits. No SvxLink file is compiled in. The layouts are described at the top of `core/include/dect2/echolink/el_proto.h`:

- **Directory (TCP 5200, one command per connection).** Log-in is `l CALL 0xAC 0xAC PASSWORD \r ONLINE3.38(HH:MM) \r LOCATION \r`. The list command is `s`, answered with `@@@`, a count, 4 lines per entry and `+++`. The location is cut to 27 characters.
- **Control (UDP audio port + 1).** RTCP with the 0xC0 header bits that EchoLink uses: an empty RR, then SDES or BYE. SDES is sent to connect and as a keep-alive every 10 s. A connect gives up after 5 tries. A peer silent for 50 s is gone.
- **Audio (UDP 5198).** RTP header `C0 03 seq 0 0`, then 4 x 33-byte GSM frames (144 bytes). `oNDATA\r...` carries info and `oNDATA CALL>msg\r\n` carries chat, each NUL-terminated. Speex packets (payload type 0x96) are ignored, because OnAir offers GSM only.

Replies go to the port the peer actually sent from, so a peer behind NAT is reached. Stations can call in only if UDP 5198 and 5199 reach this computer. The window says so.

## GSM 06.10

`core/src/echolink/gsm610.cpp` is written from the ETSI EN 300 961 (GSM 06.10) algorithm, in 16-bit fixed point. GSM full rate needs no patent licence.

- **Decoder.** Bit-exact against FFmpeg 8.0's GSM decoder on 50 frames of encoded speech and on 60 frames of random parameters, which reach the saturation paths. `tests/echolink/gsm_vectors.inc` holds those frames and FFmpeg's output hashes.
- **Encoder.** The official ETSI test sequences were not available, so the encoder is not proven bit-exact. It is checked by SNR on a speech-like signal:
  - about 9 dB at normal levels
  - at least 6 dB from -40 dBFS to near full scale, with DC offsets of +1500 and -2500
  - with added noise
  - full-scale square-wave overload and silence also pass
- **Frame layout.** The usual libgsm / RFC 3551 33-byte frame with the 0xD signature.

## Tests

`tests/echolink/test_echolink_core.cpp` (ctest `echolink_core`):

- **GSM.** The FFmpeg vectors, 2000 random pack/unpack round trips, frames without the signature, an SNR sweep over level, DC offset and noise, overload and silence.
- **RTCP.** The SDES and BYE bytes against hand-built SvxLink layouts, SDES items and PRIV, every truncation, 20000 random packets, callsign sanitising.
- **RTP and text.** The 144-byte audio packet, Speex refused, partial packets, info and chat round trips, line-break injection, a chat with no NUL.
- **Directory format.** The exact log-in, busy and off commands, redaction, callsign rules, the station list parsed at every chunk size from 1 to 40 bytes (server message, separator, kinds, busy flag and time), an incorrect-password list, a non-list reply, search and the kind filters.
- **Mock directory server.** Wrong password refused, right one accepted, list fetched, log-out on close, unreachable server reported, no password in logs or status.
- **Two clients over loopback.** Connect, names, info texts, a third caller refused while busy, 2 s of 1 kHz tone (level within 30 %, clean spectrum, no loss), chat, BYE, calls refused when turned off.
- **Fake-clock timers.** Connect gives up after 5 SDES at 50 s, keep-alives go to the NAT port, the link stays up for 3 minutes with peer keep-alives, loss, duplicates, reordering and the 16-bit sequence wrap are counted, the 50 s time-out fires, a stranger's audio is ignored.
- **Audio helpers.** The 48 to 8 kHz converters (pass band within 6 %, stop band below -50 dB), VOX timing, and every RF bridge rule (off by default, acknowledgement needed, half duplex, tail, time-out, lockout).

## Not done / limits

- Not tried on the real EchoLink network.
- No Speex.
- One QSO at a time: OnAir does not host a conference.
- No IPv6. EchoLink is IPv4.
- No EchoLink proxy or relay support. A station behind a router needs the UDP ports forwarded.
- The DTMF keypad belongs to the TX work (feat39-txfeat) and is not in this window.
- The app window was syntax-checked only. No GUI screenshot was taken in this session.
