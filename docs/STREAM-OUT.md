# Send the stream over the network (MPEG-TS)

Every digital TV mode that makes a transport stream (DVB-T, DVB-T2, DVB-S/S2, DVB-C, ATSC 1.0 and 3.0, ISDB-T, DTMB) can send it out live. In the **Stream** tab:

1. Type an address in **Address**: `udp://127.0.0.1:1234`, `udp://239.1.1.1:5000` (multicast), `rtp://192.168.1.20:5004` or `http://:8080`.
2. Tick **Send the stream over the network**.

Open it in OBS (media source), VLC or ffmpeg: `ffplay udp://127.0.0.1:1234`, `vlc http://127.0.0.1:8080/stream.ts`.

**What is sent.** The **service** list above chooses one programme (rewritten with its own PAT/PMT, so a player locks at once) or the whole multiplex. UDP and RTP carry 7 x 188 = 1316 bytes per datagram, spaced evenly to the signal's own bit rate (a short backlog from the decoder is released a little faster, never thrown away; a dead network is dropped after 5 s). RTP uses payload type 33. **remove null packets** is available. Changing the address, the service or switching off and on again is safe while running.

**HTTP.** `http://host:port/stream.ts` answers `video/mp2t` and then the live packets. It listens on this computer only, unless the address is not `127.0.0.1`/empty (for example `http://0.0.0.0:8080`), then on the network. Up to 16 viewers; a viewer that cannot keep up misses packets and is dropped after about 3 s. Advanced has an optional key (`?key=`). Without a key anyone who can reach the port can watch.

**Advanced network output** (collapsed): port, TTL, RTP, HTTP port / network / key, and the multicast adapter (`OutputConfig::iface`).

Tested: tests/tv/test_tsout_net.cpp feeds a synthetic TS at 4 Mbit/s and checks, on loopback, the 1316-byte datagrams, sync bytes, continuity counters, paced arrival, RTP header and sequence, the HTTP header and body, and a restart. Not tried with a live radio signal, nor with OBS/VLC here.
