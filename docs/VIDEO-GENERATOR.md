# Digital video generator

Tools > Digital video generator turns a camera, a still picture or a video file into an MPEG transport stream, with an overlay of your
own on the picture: a logo, the encoding parameters, free text and a band of rolling text. The stream is written to a `.ts` file or
sent over the network as UDP or RTP. Every encoder setting can be chosen, and the window estimates the bitrate the settings need, so the
stream can be fitted to a DVB link before anything is encoded.

The tool stands alone: it does not change the receiver, the transmitter or anything else in OnAir. To transmit what it makes, write a
`.ts` file and choose it in the transmitter as the "MPEG-TS / video file" (DVB-T, T2, S, S2, C, ATSC, ISDB-T, DTMB send a `.ts` as it is).

- Engine: `core/include/dect2/tv/videogen.h`, `core/src/tv/videogen.cpp` (sources, encoders, muxer, output)
- Settings and estimate: `core/include/dect2/tv/videogen_params.h`, `core/src/tv/videogen_params.cpp`
- Overlay: `core/include/dect2/tv/videogen_overlay.h`, `core/src/tv/videogen_overlay.cpp` (text with stb_truetype, logo with OnAir's PNG decoder or FFmpeg, scaled with swscale)
- Window: `app/panels/videogen_ui.cpp`; the mode selector tab `app/panels/videogen_modes_ui.cpp`
- DVB modes for the mode selector: `core/include/dect2/tv/dvb_modes.h`, `core/src/tv/dvb_modes.cpp`
- Tests: `tests/tv/test_videogen.cpp`, `tests/tv/test_dvb_modes.cpp`

Every setting in the window has a (?) with a short version of what is written here.

## Quick start

1. Open Tools > Digital video generator.
2. On **Sources and encoding**, choose the input (Camera, Still picture, Video file or Test generator) and the output (MPEG-TS file, UDP or RTP stream).
3. Leave the encoding settings at their defaults, or change them: the **Estimated bitrate** panel on the right follows every change.
4. Optionally place an overlay on the **Overlay** tab, and check on **Digital Television Mode Selector** which DVB modes carry the stream.
5. Press **Start encoding** (file) or **Start streaming** (UDP / RTP). Stop ends a stream; a file stops by itself at its length.

## Sources

| Input | What it does |
|---|---|
| Camera | A webcam, or an HDMI / SDI capture card. The list is made when the window opens (Refresh after plugging one in); the camera itself is opened only when you press Start. A camera is always encoded in real time. |
| Still picture | One picture (PNG, JPEG, BMP, ...) sent as a continuous stream at the chosen frame rate: a test card, a slide, a station logo. Length one minute unless you change it. |
| Video file | Any file FFmpeg can read. The length follows the file when you choose it. When the length is longer than the file, the file starts again. Picture and sound are decoded in step, so writing a `.ts` file goes faster than real time. |
| Test generator | A copy of the transmitter's own moving test picture: the one in the test transport stream that the TX studio's generators carry (for DVB-T2, the T2Generator's real PLP), drawn at the output resolution. **Pattern**: Test card (colour bars, a moving sweep, a chequerboard with a bouncing ball), Colour bars or Grey ramp; a white bar grows along the bottom once a second. Put a callsign, a CQ call or other text on it with the **Overlay** tab. Length one minute unless you change it; a `.ts` file is written faster than real time. Picture fit does not apply. |

**Picture fit** decides what happens when the source has another shape than the output, such as a 4:3 camera in a 16:9 picture:
**Stretch** fills the picture (shapes look wider), **Letterbox** keeps the whole source with black bars, **Crop** fills the picture and
cuts the edges.

**Sound**: silence, a 1 kHz test tone at -20 dBFS, the sound of the video file (only with a video file as input), or a microphone.

## Output

| Output | Use |
|---|---|
| MPEG-TS file (.ts) | A transport stream file. Give it to the transmitter, or play it in VLC. An existing file is replaced. |
| UDP stream | Plain transport-stream packets, 7 per datagram, sent in real time. |
| RTP stream | The same with a 12-byte RTP header (payload type 33, MPEG-TS) in front. The sequence number lets a player see lost packets. |

The default port is **5000**. The address is the receiving PC (unicast, for example `192.168.1.20`), a multicast group
(`239.0.0.0` to `239.255.255.255`, any PC on the network can join it) or `127.0.0.1` for this PC. **TTL** (multicast only) limits how
many routers the stream may cross; 1 keeps it on this network.

While streaming, the window shows the address, for example `rtp://192.168.1.20:5000`, with a **Copy** button.

### Watching the stream in VLC

On the receiving PC, choose Media > Open Network Stream and enter:

| Stream | Unicast (sent to this PC) | Multicast |
|---|---|---|
| RTP | `rtp://@:5000` | `rtp://@239.1.1.1:5000` |
| UDP | `udp://@:5000` | `udp://@239.1.1.1:5000` |

The `@` means "listen here". If nothing arrives, check the firewall of the receiving PC (UDP port 5000 inbound).

## Encoding settings

The defaults are a good start for a standard-definition DVB-T test: H.264, 720x576, 15 fps, 1 Mbit/s CBR, GOP 2 s, AAC 96 kbit/s mono.

### Video

| Setting | Default | Notes |
|---|---|---|
| TS content | Video + audio | Video only, video + audio, or video + 2 to 4 audio tracks. Every track carries the same sound on its own PID, to test receivers that offer a language choice. Audio takes part of the capacity. |
| Video codec | H.264 | H.264 is widely supported and the safe choice. H.265 needs about 30 % less bitrate, but only newer receivers (most DVB-T2 sets) decode it: check the whole chain. MPEG-2 works everywhere but needs about twice the bitrate of H.264. |
| Rate control | CBR | **CBR**: a constant rate, what a broadcast link needs. **Constrained VBR**: the rate varies with the scene up to 1.5 times the bitrate; the mux rate needs headroom for the peaks. **VBR / CRF**: a constant quality (x264 and x265 only), capped at twice the bitrate. |
| Video bitrate | 1000 kbit/s (Auto) | More bitrate generally gives a better picture but uses more of the channel. **Auto** follows a rule of thumb (see *The estimate*); untick it to set your own. |
| CRF | 23 | With VBR / CRF only. Lower is better quality and more bits: 18 is close to transparent, 28 clearly softer. |
| Frame rate | 15 fps | 1 to 30 fps. Lower rates need less data for motion but look less smooth. Some equipment takes only 25 fps (50 Hz countries) or 30 fps (60 Hz countries). |
| Resolution | 720x576 | 320x240 up to 1920x1080. A smaller picture needs less bitrate and encoder time. 720x576 is standard definition in Europe; 1280x720 and 1920x1080 are HD. |
| Frame types | I/P/B | **I/P/B**: the best compression. **I/P**: no B-frames, slightly lower delay. **I-only**: every picture stands alone; it needs much more bitrate for the same quality. |
| GOP | 2 s | The time between keyframes. A receiver can only start or recover on a keyframe: a short GOP helps after a dropout and when switching channel, a long GOP compresses better. 1 to 2 s is common. |
| Profile | Main | H.264 only. Baseline (simple decoders, no B-frames), Main (the broadcast default), High (better compression for newer receivers). |
| Pixel format | yuv420p, 8-bit | Fixed: 4:2:0 at 8 bits, which every receiver decodes. |
| Encoder preset | veryfast | x264 / x265 only. Slower presets compress better but need more CPU. For live input, choose the slowest preset that still keeps up; the window warns about frames that come late. |
| Latency tuning | off | x264 / x265 only. `zerolatency` removes look-ahead and B-frames, so a live picture arrives sooner, at some cost in compression. |

### Audio

| Setting | Default | Notes |
|---|---|---|
| Audio codec | AAC | MPEG-1 Layer II is accepted by nearly every DVB receiver. AAC sounds better at the same bitrate. AC-3 is common on HD channels. |
| Audio bitrate | 96 kbit/s | Per track. 64 to 96 kbit/s is fine for speech in mono; 128 to 192 kbit/s for music in stereo (Layer II needs the higher end). |
| Sample rate | 48 kHz | The broadcast standard; 32 and 44.1 kHz only when the receiver needs them. |
| Channels | Mono | Mono saves some bitrate; stereo needs more. |

### Transport stream

| Setting | Default | Notes |
|---|---|---|
| TS mux rate | Auto | The total rate of the stream: video, audio, tables, and null packets that pad it to a constant rate. Keep it at or below the net bitrate of the link (see the table below). **Auto** uses the estimate plus 3 %. |
| Service ID | 1 | The programme number in the PAT / PMT and the service ID in the SDT. |
| Service name, Provider | OnAir video, OnAir | What a receiver shows in its channel list (SDT). |
| PIDs | Automatic | Automatic: PMT 0x1000, video 0x100, audio 0x101 (more tracks follow). Set them yourself to test how a receiver handles particular PIDs. |

## The estimate

The panel on the right of the first tab shows, for the settings as they are:

- **Video**: the video bitrate (and the **Video peak** with constrained VBR or CRF);
- **Audio**: all tracks together;
- **PES and TS headers, PCR**: every 188-byte TS packet has a 4-byte header, every picture a PES header, and the clock reference is sent
  every 40 ms;
- **PAT, PMT, SDT**: the tables a receiver needs to find the programme, sent 10 times a second (PAT, PMT) and twice a second (SDT);
- **Total**, the **Suggested mux rate** (the peak plus 3 %) and the **Mux rate used**.

The bar shows how the mux rate is divided; the empty part at the end is null packets (padding). A red outline means the mux rate is
lower than the streams need: the muxer will run late and players may stutter.

The suggested video bitrate is a rule of thumb, not a measurement: bits per pixel per picture for each codec (H.264 0.16, H.265 0.11,
MPEG-2 0.32), times 1.15 for I/P and about 3 for I-only. A busy scene (sport, noise) needs more; a still picture much less. Use it to
see how the settings move the rate, then check the picture.

### What a DVB-T channel carries

The net bitrate of an 8 MHz DVB-T channel, in Mbit/s (EN 300 744). Set the TS mux rate at or below it.

| Modulation | Code rate | Guard 1/4 | Guard 1/8 | Guard 1/32 |
|---|---|---|---|---|
| QPSK | 1/2 | 4.98 | 5.53 | 6.03 |
| QPSK | 2/3 | 6.64 | 7.37 | 8.04 |
| 16-QAM | 1/2 | 9.95 | 11.06 | 12.06 |
| 16-QAM | 2/3 | 13.27 | 14.75 | 16.09 |
| 64-QAM | 2/3 | 19.91 | 22.12 | 24.13 |
| 64-QAM | 3/4 | 22.39 | 24.88 | 27.14 |

A 7 MHz channel carries 7/8 of these, a 6 MHz channel 6/8. The **Digital Television Mode Selector** tab has every mode of every system.

## Codecs and FFmpeg

The window lists only the encoders that really work on this computer: each one is tried when the window first opens. MPEG-2 video is
always there and is used when the chosen encoder is missing.

- The FFmpeg that comes with the OnAir packages is built under the LGPL and has MPEG-2 video, MPEG-1 Layer II and AAC. H.264 and H.265 are
  shown greyed out with "not in this FFmpeg".
- OnAir built against the system's FFmpeg (most Linux distributions) usually has libx264 and libx265 as well, and AC-3.
- Hardware encoders (NVIDIA NVENC, Intel Quick Sync, AMD AMF, Apple VideoToolbox, Windows Media Foundation) are listed when the build
  has them and the hardware is there. Preset, latency tuning and CRF are x264 / x265 options; they are greyed out for other encoders.
  VAAPI encoders are not offered: they need the pictures on the GPU.

## Overlay

The **Overlay** tab places things on the picture before it is encoded.

- **Blocks**: **Logo** (a PNG file, transparency kept, scaled into its box keeping its shape; other picture formats work when the FFmpeg in the build decodes them), **Free text** (more lines allowed), **Rolling text** (a band with
  text running through it), and every **parameter** of the first tab, grouped as there (Video, Audio, Transport stream). A parameter
  block shows its current value and follows it; its tooltip in the list says where it comes from.
- **Place**: drag a block from the list onto the picture, or double-click it to put it in the middle. Drag a block to move it; drag
  the blue corner to resize a logo or the rolling band. **Delete** removes the selected block.
- **Magnetic**: a block snaps to the edges of the other blocks (side by side or aligned), to the edges of the picture and to its
  centre lines, when it comes within a few pixels; yellow lines show the snap. Hold **Alt** to place it freely, or untick Magnetic.
- **Per block** (on the right): text or label, font size in pixels of the encoded picture, colour, a dark box behind the text,
  position. For the logo, the PNG file and its size. For rolling text, the **direction** (left to right or right to left) and the
  **speed** in characters per second (default 1, left to right).
- The faint inner frame is the title-safe area (90 %): some TV sets cut the edges, so keep text inside it.

What the canvas shows is drawn by the same code as the encoded picture. While encoding, the canvas shows the encoder's own pictures,
and changes to the overlay go out on the next frame.

The overlay text uses the font of the OnAir interface (Inter). The layout is kept in `videogen_overlay.txt` and the other settings in
`videogen.txt`, both in the OnAir data folder.

## Digital Television Mode Selector

The third tab answers: with this bitrate, this much room on the spectrum and this signal quality, which DVB modes will work, and which
is the best? It knows every standard mode of DVB-T, DVB-T2, DVB-T2 narrow band (the 1,7 MHz channel, a block of its own), DVB-S,
DVB-S2 and DVB-S2X.

**Left, the link:**

- **Bitrate**: what has to be carried. By default the TS mux rate of the first tab (*The generator's TS mux rate*); untick it to type
  your own, in Mbit/s.
- **Bandwidth**: the room you have on the spectrum, in MHz: a channel, or a segment of the band plan.
- **C/N**: the carrier-to-noise ratio you see on the spectrum analyser, in dB, measured in the same bandwidth as the signal.
- **Most optimal mode**: the mode with the smallest bandwidth that carries the bitrate at this C/N, over all systems and with the
  parameters set in their blocks. Between modes of the same bandwidth it takes the one with the most C/N margin. Its parameters,
  bandwidth, symbol rate (satellite), capacity, the C/N it needs and the margin are listed. Hover its name for the parameters in words.

**Right, a block per system.** On the left of a block are the system's parameters (channel, FFT, guard interval, pilot pattern,
modulation, code rate, FEC frame, pilots, roll-off ...), each on **Any** or set to the value you want; **All on Any** clears them. On the
right are the modes that still work with those parameters, the best first: their bandwidth, symbol rate (satellite), bitrate, the C/N
they need and the margin. When none is left the block says why: no such mode, none carries the bitrate in the bandwidth, or the C/N is
too low (with the least the parameters need).

How the modes are worked out (the formulas and standard tables of the workbook `DVB_transmission_modes.xlsx`):

| System | Bandwidth | Bitrate | Minimum C/N (AWGN) |
|---|---|---|---|
| DVB-T (EN 300 744) | K / Tu of the channel | data carriers x bits x code rate x 188/204 / symbol time | Annex A Table A.1 |
| DVB-T2 and DVB-T2 1.7 MHz (EN 302 755) | Ktotal / Tu of the channel | data cells x bits x (Kbch - 80) / Nldpc / symbol time | TS 102 831 Tables 44/45 + pilot boost (Table 46) |
| DVB-S (EN 300 421) | symbol rate x 1.35 | symbol rate x 2 x code rate x 188/204 | Eb/N0 of Table 3, as C/N |
| DVB-S2 (EN 302 307-1) | symbol rate x (1 + roll-off) | symbol rate x (Kbch - 80) / PLFRAME | Es/N0 of Table 13 |
| DVB-S2X (EN 302 307-2) | symbol rate x (1 + roll-off) | symbol rate x (Kbch - 80) / PLFRAME | Es/N0 of Tables 20a-20c |

- A terrestrial mode has a fixed channel, so a fixed bandwidth and bitrate. A satellite mode has a free symbol rate: its bandwidth is
  what the bitrate needs, and its capacity what the whole bandwidth would carry.
- The thresholds are ideal: AWGN channel, no implementation margin, phase noise or multipath. Keep a few dB in hand; the margin column
  shows how much there is.
- The DVB-T2 bitrate is the ideal cell capacity: P1 and P2 symbols, signalling and padding are not deducted, so the real one is a few
  percent lower. The DVB-T and DVB-T2 thresholds were simulated for 8 MHz and are used for the other channels as well.
- DVB-S2 short frames have no threshold in the standard: they are listed in the counts but never as available.
- The link inputs and the parameters are kept in `videogen_modes.txt` in the OnAir data folder.

## When something goes wrong

| What you see | What to do |
|---|---|
| "No camera found" | Plug it in and press Refresh. On Linux, check that your user may open `/dev/video*` (group `video`). |
| H.264 / H.265 greyed out | This FFmpeg has no such encoder (see *Codecs and FFmpeg*). Use MPEG-2, or build OnAir against a system FFmpeg with libx264. |
| "frames late" while streaming | The encoder is too slow for real time: a faster preset, a lower resolution or frame rate, or a hardware encoder. |
| VLC shows nothing | Check the address on the receiving PC (`rtp://@:5000`), the firewall, and that both sides use the same protocol (RTP or UDP). |
| The mux bar has a red outline | The mux rate is lower than the streams need: raise it, or lower the video bitrate. |
| The picture breaks up on the receiver | The stream is larger than the link: compare the mux rate with the table of DVB-T bitrates, or check the mode on the Digital Television Mode Selector tab. |
