# Live UDP transport stream into the transmitters

Digital TV transmit modes take an MPEG-TS stream sent to OnAir over UDP, for example from OBS, ffmpeg or VLC.
In the transmit panel, put `udp://@:1234` in the MPEG-TS / video file field (or press "Listen on udp://@:1234" under "Live UDP stream").

Address forms: `udp://@:1234` (all interfaces), `udp://127.0.0.1:1234` (one interface), `udp://239.1.1.1:1234` (join a multicast group), `rtp://@:5004`. Options after `?` are ignored.

Sender examples:
`ffmpeg -re -i video.mp4 -c:v libx264 -c:a aac -f mpegts udp://127.0.0.1:1234?pkt_size=1316`
OBS: Settings > Stream > Custom, server `udp://127.0.0.1:1234` (MPEG-TS output).

What it does:
- Datagrams of any size are accepted (7 x 188 bytes is typical). The bytes are re-aligned to 188-byte packets on their sync bytes (three in a row); lost or cut datagrams cost a re-sync, counted.
- A 12-byte RTP header (payload type 33) is detected and removed.
- A buffer of 6000 packets absorbs jitter. Output starts when 64 packets are queued. When the modulator asks and the buffer is empty, a null packet (PID 0x1FFF) is sent, so the radio's rate and the receivers' clocks are kept. When the buffer is full the oldest packets are dropped.
- The transmit panel shows packets in, queued, null packets inserted, dropped and re-syncs.
- The stream must fit the channel's bit rate: a stream faster than the modulator overflows and drops packets (picture breaks up). Encode below the channel's net rate.
- OnAir only listens; nothing is sent anywhere.

Modes that take it (the ones that take a transport-stream file): DVB-T, DVB-T2, DVB-S, DVB-S2 (also QO-100 DATV), DVB-C, ATSC, ISDB-T, DTMB.
Code: core/include/dect2/tv/ts_udp_in.h, test `ts_udp_in`.
