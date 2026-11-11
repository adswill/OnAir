# EchoLink client (3.9)

Tools > EchoLink... opens the window. It is a network client, not a radio mode, so it has no mode number and no mode-list entry.

**Validation status: mock-tested only.** Every test talks to a mock directory server, a mock proxy, a simulated network or other OnAir clients on 127.0.0.1. Nothing here has contacted the real EchoLink servers, a real proxy or a real EchoLink station. The first real log-in is the user's.

## What it does

- **Log in.** Uses the callsign, password, name and location from the window. The one button "Log in and list stations" opens the UDP ports, logs in and fetches the list. While logged in, the log-in is repeated every 5 minutes, as SvxLink does. Closing the program or pressing Log out sends the OFF status.
- **Station list and search.** Shows the list with search by callsign, description or node number, and a filter for stations, links (-L), repeaters (-R) and conferences (`*NAME*`). Double-click a row, or select it and press Connect.
- **Connect.** Connects to a station, link, repeater or conference. Advanced has "Connect to this address" for a raw IPv4 address. Incoming calls are accepted unless "Accept calls from other stations" is off. A second caller during a QSO gets a BYE.
- **Audio.** GSM 06.10 full rate both ways, 4 frames (80 ms) per RTP packet. Transmit from the microphone uses "Hold to talk", "Talk (latched)" or VOX (threshold and hold time under Advanced). Received audio plays on the speakers with a volume slider. The microphone opens only while connected.
- **Chat and info.** Sends and receives text chat. Shows the station's info text, which for a conference lists who is connected.
- **RF link** ("Link to RF channel"). Off by default and off again at every start. The first time it is turned on, a note about licences, unattended-operation permits, identification and -L/-R registration must be acknowledged. It links a radio on a sound card. The radio's receive goes over EchoLink when it rises above the squelch level. EchoLink audio is played into the radio's input and keys the radio, either through the radio's own VOX or through Hamlib (CAT, RTS or DTR). It works half duplex: the side that started keeps the channel. There is a tail time and a transmit time-out (default 180 s). After a time-out the radio waits until the EchoLink side is quiet. The bridge rules are unit-tested. Using it with a real radio is not tested.

## Hosting a conference

Advanced > "Host a conference" makes this station a conference: every station that connects joins, and everyone hears everyone. "Stations at most" sets the limit (10 by default, 2 to 50). Use the conference's own callsign (`*NAME*`), registered for it at echolink.org. The setting takes effect at the next log-in.

- **Joining.** Incoming stations are accepted while there is room. A station past the limit gets a BYE, and the log says the conference is full. "Add to the conference" calls a station from the list, and that station joins too.
- **Station list.** The Connection section shows a table of the connected stations: callsign, name, whether each is talking, listening or still being called, and packets in and lost. Each row has a Disconnect button. "Disconnect everyone" ends the conference.
- **Info text.** Every station sees the conference's info text, which lists who is connected: `Connected stations (N):`, then one `CALL  Name` line per station. It is sent again whenever someone joins or leaves.
- **Audio mixing.** Every 80 ms each station's received GSM audio is decoded. Each station then gets the sum of everyone else plus the host's microphone, with saturation, encoded by an encoder of its own. A station never hears itself. Two people talking at once both reach the others. The host hears the sum of all stations. A station's audio waits for a whole 80 ms block, which absorbs network jitter; after 160 ms of silence its last part is mixed and padded.
- **Chat.** Chat from a station is passed on to the others under the sender's callsign. The host's chat goes to everyone.
- **Leaving.** A BYE, a 50 s silence or the host's Disconnect removes a station. Keep-alives run per station. A station that reconnects from a new port replaces its old entry.
- **Addressing.** Stations are told apart by address and port, so two stations behind one NAT address both work.

The code is `core/src/echolink/el_conference.cpp`. Each station is an ordinary `Session`. The behaviour follows SvxLink's ModuleEchoLink (several QSOs, the list in the info text, chat passed on). The mixing is OnAir's own.

## Proxy (relay)

For a computer whose UDP ports 5198 and 5199 cannot be reached (behind NAT or a firewall that cannot forward them, or a carrier that blocks them), Advanced > "Proxy (relay)" sends all EchoLink traffic through one TCP connection to an EchoLink proxy. Set the proxy host, its port (8100 by default) and its password. An empty password means a public proxy, and the password "PUBLIC" is sent, by convention. When a proxy is set, no local UDP port is opened. The status line shows the proxy state.

The protocol follows SvxLink's EchoLinkProxy.cpp. It is laid out in `core/include/dect2/echolink/el_proxy.h`:

- **Greeting.** The proxy sends an 8-byte nonce. The client answers with `CALLSIGN \n` and then the 16 raw bytes of MD5(upper-case password + nonce).
- **Blocks.** After that, blocks run both ways: type (1 byte), IPv4 address (4 bytes, network order), length (4 bytes, little endian), then the data.
- **Block types.** UDP_DATA (5) and UDP_CONTROL (6) carry the audio-port and control-port packets. The far end is always 5198 / 5199, so no port is carried.
- **Directory.** The directory log-in and the station list go through TCP_OPEN (1) to the directory server's address. The proxy answers TCP_STATUS (4), and 0 means open. Then come TCP_DATA (2) and TCP_CLOSE (3). The client resolves the directory host itself.
- **Errors.** A SYSTEM (7) block reports a bad proxy password (1) or access denied (2). The window shows it, and the connection is not used further.

The proxy password is stored under `echolinkProxyPassword`, which is a credential key, so it is kept out of plain backups. The field never shows it, and it is not logged.

If the proxy drops the connection, the QSO ends and the window says so. OnAir does not reconnect by itself: log out and log in again.

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

- **Decoder.** Bit-exact against FFmpeg 8.0's GSM decoder on 50 frames of encoded speech (with a clipped burst) and on 60 frames of encoded noise and stepped tones. `tests/echolink/gsm_vectors.inc` holds those frames and FFmpeg's output hashes. Random parameter frames were tried and are not a usable reference: they overflow the filters, and there the standard saturates while FFmpeg wraps around. A lag outside 40 to 120 also differs: FFmpeg clamps it, while section 5.3.2 repeats the last lag. OnAir follows the standard in both cases. The saturation paths are covered by the overload test.
- **Encoder.** The official ETSI test sequences were not available, so the encoder is not proven bit-exact. It is checked by SNR on a speech-like signal:
  - about 9 dB at normal levels
  - at least 6 dB from -40 dBFS to near full scale, with DC offsets of +1500 and -2500
  - with added noise
  - full-scale square-wave overload and silence also pass
- **Frame layout.** The usual libgsm / RFC 3551 33-byte frame with the 0xD signature.

## Tests

`tests/echolink/test_echolink_conf.cpp` (ctest `echolink_conf`, about 4 s), on simulated networks and 127.0.0.1 only:

- **Conference on a simulated network** with a fake clock:
  - 5 stations join, and all five calls appear in every station's info text.
  - A sixth station is refused because the conference is full.
  - Two stations talk at once (600 Hz and 1400 Hz). Each talker hears only the other, at about 6000 of the 6000 sent, and its own tone below 10. The three listeners and the host hear both.
  - The host's 1 kHz reaches all five.
  - Chat is passed on without an echo, and host chat arrives.
  - A BYE removes a station and the list is sent again.
  - A station that stops sending times out after 50 s while the others stay.
  - Kicking one station, and disconnecting everyone.
- **NAT and impairments.** Two stations behind one address (different ports) and a third station, in 3 trials:
  - clean
  - 10 % audio loss with 5 % duplicates
  - the same plus 10 % reordering

  The other two always hear the talker (amplitude 5255 to 6263 of 6000 over the received audio), and the talker gets nothing back. Losses are counted.
- **Loopback conference.** Real UDP clients: two call in and the host adds a third. The info text lists 3. A 900 Hz tone from one station reaches the other two and the host (6638 to 6893 of 7000), and nothing comes back to the sender. Chat is passed on. A fourth station is refused at a limit of 3. A kick works, and closing sends a BYE to all.
- **Proxy bytes.**
  - the block layout
  - the auth reply against MD5 computed separately
  - `md5("abc")`
  - an empty password sent as PUBLIC
  - 20 blocks of every type parsed at every chunk size from 1 to 40 bytes
  - an unknown type and an oversized block refused
- **Client behind a mock proxy.** The mock proxy checks the MD5 log-in and relays UDP to a direct client and TCP to a mock directory:
  - Log-in and the station list go through TCP_OPEN, one per command.
  - The QSO works with names and info both ways.
  - A 1100 Hz tone goes both ways (7046 of 7000).
  - Chat, the BYE and the OFF log-out go through the proxy.
  - No password appears in logs or status.
  - A wrong proxy password is reported as "bad proxy password", and the log-in then fails plainly.
  - An unreachable proxy is reported.

`tests/echolink/test_echolink_core.cpp` (ctest `echolink_core`):

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
- Without "Host a conference" there is one QSO at a time. A conference mixes GSM audio only. It is tested on a simulated network and on loopback, not with real EchoLink stations.
- No IPv6. EchoLink is IPv4.
- The proxy client is tested only against OnAir's mock proxy, not a real EchoLink proxy. It does not reconnect by itself after the proxy drops the connection. OnAir cannot act as a proxy for others.
- The DTMF keypad belongs to the TX work (feat39-txfeat) and is not in this window.
- The app window was syntax-checked only. No GUI screenshot was taken in this session.
