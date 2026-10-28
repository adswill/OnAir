# Internet TV (IPTV)

OnAir can play television that is streamed over the internet, next to the antenna modes. Pick **Internet TV (IPTV)** in the mode list (TV group). No radio is needed.

**OnAir is a player only.** It comes with no channels and no channel addresses. You add a playlist or a stream address you are allowed to use, and you are responsible for what you open. OnAir does not open DRM protected streams (Widevine, PlayReady, FairPlay, SAMPLE-AES and the like): it says so and stops, it never tries to get around the protection.

## What you can open

| Source | Works | Notes |
|---|---|---|
| Playlist, M3U / M3U8 with `#EXTINF` (`tvg-id`, `tvg-name`, `tvg-logo`, `group-title`), a web address or a file | yes | 50,000 channels are fine. Relative addresses, a byte order mark, CR LF line ends, `'` and `"` quoting, `#EXTGRP`, `#EXTVLCOPT` (user agent, referrer), `#EXTHTTP`, Kodi `url\|User-Agent=..`, and plain lists of addresses are understood. Exact duplicates are left out. |
| HLS (`.m3u8`), live and on demand | yes, MPEG-TS segments | Several bitrates: automatic from the measured speed (Advanced: fixed variant). Discontinuities, redirects, byte ranges, AES-128 segment encryption (the key address comes from the playlist: this is ordinary transport encryption, not DRM). The audio and subtitle renditions the stream lists are shown. |
| MPEG-TS over HTTP / HTTPS | yes | live streams and files |
| UDP, RTP, multicast (`udp://@239.1.1.1:1234`, `rtp://...`) | yes | IPv4. Advanced: the network adapter for the multicast join. |
| HLS with fragmented MP4 (CMAF) segments, MP4 files | not yet | OnAir says so |
| DASH (`.mpd`) | not yet | OnAir says so |
| RTSP | not yet | OnAir says so |
| Streams with a separate audio rendition (video and audio in different playlists) | not yet | |
| DRM protected | never | |

Playback goes through the same player as the antenna TV: volume, mute, subtitles, audio track, pause and the live buffer, full screen with its keys, pop-out window, recording to a file. The screen shows the stream type, resolution, codec, download speed, buffered seconds and how often it had to reconnect.

## Channel list

On the right: search, group, favourites (right-click a channel, or the button under the picture), the last watched channels on top, logos (PNG logos are fetched when a row is shown and kept in the data folder; other image types show no logo). Playlists are remembered with a copy of their last download, so the list is there on the next start without a network.

## Programme guide

Give the address (or file) of an XMLTV guide, or leave it: a playlist that names its guide in `url-tvg=` fills it in. The programmes are matched to the channels by `tvg-id`, then by name. The time zone of every programme is honoured. A `.gz` guide is not unpacked yet: use the plain `.xml` address.

## When something does not play

OnAir never ends in a bare "failed". It shows the real reason (for example *403 Forbidden*, *404 Not Found*, *certificate problem*, *no data for 10 seconds*) and tries again by itself with growing waits (1, 2, 3, 6, ... up to 15 seconds), the picture carries on when a single segment is missing, and the screen says "reconnecting" with the attempt number. If a provider wants a particular player name or a login, set **Advanced**: User-Agent, Referer, extra headers. **Ignore certificate errors** is off by default and is never turned on by itself.

## HTTPS and the FFmpeg inside OnAir

The FFmpeg that OnAir ships (`tools/package/build_ffmpeg_static.sh`) is built with `--disable-network` and `--disable-everything` plus a short list of transport stream decoders. It contains no network code and no TLS, on purpose: it keeps the licence simple (LGPL-2.1-or-later, no OpenSSL, GnuTLS or other library inside it) and the attack surface small. OnAir therefore fetches the bytes itself (`core/src/tv/iptv_*.cpp`) and gives the player an MPEG transport stream, exactly like a recording. No patented codec is added: the video and audio are decoded by the same decoders as for the antenna.

TLS (https) comes from the system on each platform:

| System | TLS | Licence |
|---|---|---|
| macOS | NSURLSession (Apple's own TLS and the system's trusted certificates) | system framework |
| Windows | WinHTTP (Schannel, the Windows certificate store) | system library. **Needs a test on a Windows computer.** |
| Linux | the system's OpenSSL 3 (libssl3), used when it was installed at build time; certificates from `/etc/ssl` or `SSL_CERT_FILE` | Apache-2.0, compatible with GPL-3 and linked dynamically. **Needs a test on Linux packages:** a `.deb` needs `libssl3` as a dependency. Without it https says so plainly and http, udp and rtp still work. |

## Where to find channels

OnAir has no list of its own. The community project **iptv-org** (https://github.com/iptv-org/iptv) keeps playlists of publicly available streams; GitHub reports its licence as the Unlicense (public domain), and its README says the links point to streams their owners made public and that it has no control over the destinations. That is a text pointer only: OnAir does not download from it, bundle it or link it inside the program. Whether a given stream may be watched where you live is for you to check.

## Not done in this version

RTSP, DASH, CMAF / fMP4 HLS, separate audio renditions, gzip guides, `onair://` links and dropping a playlist on the window, re-serving the stream from OnAir Web.
