# Dolby Atmos and Dolby TV sound in OnAir 3.9

Request: a user with a Windows laptop (ASUS 2025) that has Dolby Atmos asked for Atmos support, mainly for ATSC 1.0, ATSC 3.0 and DVB-T. This page covers what can legitimately be done and what was implemented. It also covers what cannot be done, and why.

## Before 3.9: what a user heard and saw

All the TV modes share one player (`core/src/tv/player.cpp`): DVB-T/T2, ATSC 1.0, ISDB-T, DTMB, DVB-S/S2, QO-100 DATV, IPTV and ATSC 3.0 after its remux. The player decodes sound with the FFmpeg inside OnAir. AC-3 and E-AC-3 patents have expired, and FFmpeg has had its own decoders for them for years. Every decoded track was resampled to **stereo**, 48 kHz (`AV_CHANNEL_LAYOUT_STEREO`). The sound output (`AudioOut`) only handled stereo.

- **DVB-T/T2 or DVB-S with E-AC-3 + Atmos (JOC).** FFmpeg decoded the 5.1 (or 7.1) channel bed and ignored the objects. The bed was mixed down to stereo. The info row said `eac3  6 ch`, with no mention of Atmos.
- **ATSC 1.0 (AC-3, A/52).** The 5.1 was mixed to stereo, and the info row said `ac3  6 ch`.
- **ATSC 3.0 (AC-4).** Without the user-installed AC-4 FFmpeg (docs/CODECS.md), the sound was left out with a note. With it, the AC-4 was decoded by that library, re-encoded as AC-3 stereo or 5.1 (`ac4_xcode.cpp`), and played as stereo. Nothing showed whether the AC-4 was immersive.
- **DVB AC-4 (descriptor 0x7F/0x15).** Not recognised in the PMT, and there was no sound.

## The facts (with sources)

- **E-AC-3 JOC ("Dolby Atmos" in Dolby Digital Plus).** ETSI TS 103 420 V1.2.1 (2018-10) describes it as a backwards-compatible extension of a channel-based E-AC-3 stream. A JOC decoder rebuilds up to 16 objects from the downmix. The OAMD and JOC side data travel in an EMDF container (payload ids 11 and 14) in the last dependent substream (clause 8.2).
  - The `addbsi` field of that substream carries `reserved(7)`, `flag_ec3_extension_type_a(1)` and `complexity_index_type_a(8)` (clause 8.3.1).
  - The complexity index is the number of bed, ISF and dynamic objects, at most 16 (clause 8.3.2.2).
  - The same three fields end the `dec3` box (ETSI TS 102 366 V1.4.1 annex F.6). In DASH they are `tag:dolby.com,2018:dash:EC3_ExtensionType:2018` = `JOC` and `...EC3_ExtensionComplexityIndex:2018` (TS 103 420 annex D.2.2).
  - <https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf>
- **AC-4 immersive.** ETSI TS 103 190-2 V1.3.1 (2025-07) annex E.6 / E.10 defines `ac4_dsi_v1()`. Each presentation has `b_presentation_channel_coded`, which is 0 when the presentation contains object or A-JOC substreams.
  - It also has `dsi_presentation_ch_mode` (table 56): 0 mono to 15 22.2. Modes 9 to 15 have height channels: 5.0.2, 5.1.2, 7.0.4, 7.1.4, 9.0.4, 9.1.4 and 22.2.
  - An `immersive_audio_indicator` sits near the end of the presentation. It is informative only.
  - <https://www.etsi.org/deliver/etsi_ts/103100_103199/10319002/01.03.01_60/ts_10319002v010301p.pdf>
- **ATSC 3.0.** ATSC A/342-2:2025-07 constrains AC-4 for ATSC 3.0 and contains no separate "Atmos" signalling: immersive audio is whatever the AC-4 presentation carries.
  - In practice, Dolby's NextGen TV page names loudness and dialogue enhancement as the AC-4 features broadcasters have adopted so far. I found no list of US stations airing Atmos.
  - South Korea uses MPEG-H, not AC-4.
  - <https://professional.dolby.com/en-gb/technologies/ac-4/nextgen-tv/>
- **ATSC 1.0.** A/52 AC-3, up to 5.1. I found no evidence of Atmos on ATSC 1.0 broadcasts. Whether A/53 allows E-AC-3 (stream type 0x87, descriptor 0xCC) in ATSC 1.0 was not verified.
- **DVB.** ETSI EN 300 468 V1.20.1 annex D.
  - The `enhanced_AC-3_descriptor` (0x7A) has no Atmos or JOC field. Atmos is only visible in the E-AC-3 stream itself.
  - The AC-4 descriptor is extension descriptor 0x7F with tag extension 0x15: `ac4_config_flag`, `ac4_toc_flag`, `ac4_dialog_enhancement_enabled`, `ac4_channel_mode` (mono, stereo, multichannel), then optional `ac4_dsi_byte`s.
  - ETSI TS 101 154 V2.10.1 has no "Atmos" or "JOC" text.
  - <https://www.etsi.org/deliver/etsi_en/300400_300499/300468/01.20.01_60/en_300468v012001p.pdf>
- **IEC 61937 (bitstream over S/PDIF and HDMI).** These values were cross-checked byte for byte against FFmpeg's spdif muxer (see the tests).
  - Each burst starts with Pa = 0xF872, Pb = 0x4E1F, Pc = the data type and Pd = the length.
  - AC-3 uses data type 1 with bsmod in bits 8-10 of Pc, Pd in bits, and a 1536-frame period at the stream rate.
  - E-AC-3 uses data type 0x15, Pd in bytes, and a 6144-frame period on a link at four times the rate (192 kHz), with six audio blocks per burst.
- **Windows.**
  - <https://learn.microsoft.com/windows/desktop/CoreAudio/representing-formats-for-iec-61937-transmissions> describes passthrough via `WAVEFORMATEXTENSIBLE_IEC61937` in WASAPI exclusive mode.
    - `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` = 00000092-0000-0010-8000-00aa00389b71.
    - `_DOLBY_DIGITAL_PLUS` = 0000000a-0cea-0010-8000-00aa00389b71. DD+ is 2 channels × 16 bits at 192 kHz.
  - <https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound> describes Spatial Sound (`ISpatialAudioClient`). It takes objects and a bed up to 8.1.4.4 that the app supplies itself, and the renderer the user picked encodes them (Dolby Atmos for Headphones or built-in speakers, DTS, Windows Sonic). Apps that use Media Foundation "can play Dolby Atmos content with no additional implementation".
  - Windows' own Dolby decoder MFT decodes only independent substream 0 (up to 7.1), so it does not use the JOC data either. <https://learn.microsoft.com/en-us/windows/win32/medfound/dolby-audio-decoder>
- **macOS.** Apple's Mac User Guide (<https://support.apple.com/guide/mac-help/mchle3a1461c>) says HDMI passthrough of AC-3, E-AC-3 and Atmos works on Apple-silicon Macs with HDMI, in Apple's own apps. Which CoreAudio formats a third-party app gets for E-AC-3 was not confirmed.
- **Linux.** For HDMI and S/PDIF passthrough, Kodi (`AESinkALSA.cpp`) opens ALSA with channel status AES0=0x06 (non-audio), AES1=0x82, AES2=0x00, and AES3 set to the link rate (0x02 for 48 kHz, 0x0e for 192 kHz).

## The rules this follows

OnAir ships no patented codec. Users install their own (docs/CODECS.md). Dolby's object renderers (the JOC decoder and the AC-4 A-JOC and object rendering) are Dolby's licensed technology. They are not public, and they cannot be written here. FFmpeg's E-AC-3 decoder plays the channel bed of a JOC stream and ignores the objects. Everything below either reads public header fields, plays what FFmpeg decodes, or passes the bitstream on untouched, with no codec involved.

## What 3.9 does

1. **Detection and display.** Code: `core/src/audio/dolby_audio.cpp` and `app/panels/tv_dolby_text.h`.
   - The player reads the header of every AC-3 and E-AC-3 frame of each audio track.
     - It reads the TS 102 366 annex E bsi up to `addbsi`, which gives the Atmos flag and the complexity index.
     - It reads the channel bed from acmod/LFE plus the `chanmap` of the dependent substreams, so 5.1 + back pair is shown as 7.1.
     - The bed only changes when two programs in a row agree, so a single damaged frame does not flip it.
   - The format is shown where the sound is described:
     - in the TV panel's audio track list: "Dolby Atmos (E-AC-3 JOC), 5.1 bed", "Dolby Digital Plus (E-AC-3) 5.1" or "Dolby Digital (AC-3) 2.0";
     - in the player's Audio row, with what goes to the device;
     - in the QO-100 and IPTV players.
     - When Atmos plays without passthrough, a line says the objects need a renderer.
   - The modes are DVB-T/T2, DVB-S/S2, ATSC 1.0, ISDB-T, DTMB and QO-100/IPTV. ISDB-T and DTMB normally carry AAC or DRA, so the Dolby line simply does not appear there.
   - **ATSC 3.0.** The `dac4` box of the init segment is parsed (`parseAc4DsiV1`). A presentation with objects or A-JOC, or with height channels (ch_mode 9-15), is reported in the receiver tab as "Dolby Atmos (AC-4, 7.1.4)" or "Dolby Atmos (AC-4, objects)".
   - **DVB.** The PMT parser (`rust/onair_ts/src/si.rs`) now recognises the AC-4 descriptor (0x7F/0x15). `parseAc4Descriptor()` reads its channel mode and DSI. A DVB AC-4 track without the optional AC-4 FFmpeg now shows the existing "needs an FFmpeg with the AC-4 decoder" note instead of being silent.
2. **The bed as 5.1 / 7.1** (Settings > Audio > TV sound speakers).
   - **Automatic (the default)** plays 5.1 or 7.1 when the output device has the speakers, and a stereo downmix otherwise.
     - On Windows the device's mix format decides.
     - On macOS the speaker setup in Audio MIDI Setup decides; only channels with loudspeaker labels count.
     - Linux cannot tell, so Automatic stays stereo there, and 5.1 or 7.1 can be chosen.
   - **Fixed choices:** Stereo, Stereo with a Dolby Surround Lt/Rt matrix (FFmpeg's `matrix_encoding`, for an analogue surround decoder), 5.1 and 7.1. A 5.1 stream is never spread over 7.1.
   - `AudioOut` now takes 2, 6 or 8 channels in WAVE/FFmpeg order (FL FR FC LFE SL SR, and BL BR for 7.1). The backends tell the system what each channel is:
     - CoreAudio channel layout;
     - WASAPI channel mask, with the system downmix allowed;
     - ALSA reordered to its FL FR RL RR FC LFE order;
     - miniaudio's default map.
   - Every other mode keeps stereo: the 2-channel path behaves exactly as before.
3. **Bitstream passthrough. EXPERIMENTAL, NOT TESTED ON REAL HARDWARE: no receiver was available.** Settings > Audio > "Pass Dolby audio through to my receiver (bitstream, experimental)", off by default.
   - The AC-3 or E-AC-3 packets, Atmos included, are packed as IEC 61937 without decoding (`Iec61937Packer`) and sent to the device:
     - **Windows:** WASAPI exclusive mode with the IEC 61937 Dolby Digital or Dolby Digital Plus format. Windows must allow exclusive mode for the output.
     - **macOS:** hog mode, mixing off, and the stream switched to a non-mixable `kAudioFormat60958AC3` or 16-bit non-mixable PCM format.
     - **Linux:** ALSA `hdmi:` (or the device set under "Passthrough output") with AES0=0x06, AES1=0x82, AES2=0x00 and AES3 set to the link rate.
   - The decoded stereo still runs the A/V clock, and the bursts follow it in lockstep (`AudioOut::startBitstream`). If the two drift apart by more than 2.5 bursts, a whole burst is dropped or waited for.
   - When the device refuses, the player says why and plays the decoded sound instead.
   - The volume and mute: mute sends silence. The volume is set on the receiver.
   - It works only with devices that accept a bitstream (an HDMI or optical receiver or soundbar), not with a laptop's own speakers.
4. **The laptop's own Atmos (Dolby Access).** On the user's ASUS, Atmos on the built-in speakers is Dolby Access's renderer behind Windows Spatial Sound.
   - **An E-AC-3 JOC bitstream cannot be handed to it.** Windows offers no public path from an application's bitstream to Dolby Access. Only Microsoft's own Media Foundation playback path does that, and its documented Dolby decoder MFT skips the dependent substreams.
   - **ISpatialAudioClient does accept objects**, but OnAir would first have to decode the JOC objects (positions from OAMD, signals from the JOC matrices). That decoder is Dolby's licensed technology. A clean-room implementation of TS 103 420 is legally doubtful (patents) and could not be validated without real streams, so it was not attempted.
   - What does work: OnAir plays the 5.1 bed (stereo on a laptop whose mix format is stereo), and Dolby Access's own processing applies to it as to any other app's sound.
   - Playing the bed through Spatial Sound as static 5.1/7.1 channels (no objects) would be possible in a later version, but it is not Atmos and was not done here.

## Not possible, and why

- **Rendering the Atmos objects (E-AC-3 JOC, AC-4 A-JOC and objects) inside OnAir.** This needs Dolby's licensed renderer. FFmpeg does not have one, and OnAir never ships patented decoders.
- **Feeding the laptop's Dolby Access with the Atmos stream.** Windows has no public API for it, see 4 above.
- **AC-4 passthrough.** IEC 61937-14 defines AC-4 bursts (data type 24), but no receiver test was possible. Also, ATSC 3.0 sound reaches the player only after the AC-4 is decoded and re-encoded as AC-3. Only that AC-3 (not Atmos) could be passed through.
- **Atmos for a service that is not playing.** DVB signals no Atmos flag in the PMT (see above). It is found in the sound itself once the service plays.

## Tests

`tests/audio/test_dolby_audio.cpp` (ctest `dolby_audio`):

- **E-AC-3 headers.** 1 920 combinations built bit by bit from TS 102 366 annex E: stream type × acmod × LFE × block count (1, 2, 3, 6, plus reduced rate) × mixing and informational metadata × the Atmos flag, with random values in every skipped field.
- **AC-3.** All 38 frame size codes at 48 and 32 kHz against A/52 table 5.18, and the 44.1 kHz rounding.
- **Beds.** 5.1 + dependent Lrs/Rrs = 7.1, and 7.1 + Vhl/Vhr = 7.1.2. A 40-program stream was fed in random splits with false-sync junk between frames.
- **Real frames.** Frames from FFmpeg's AC-3 and E-AC-3 encoders are parsed.
  - An Atmos flag inserted into real E-AC-3 frames is read back.
  - FFmpeg's decoder gives bit-identical sound with and without it, so the flag sits where the decoder skips it.
  - On FFmpeg 7.1+ the decoder itself reports `AV_PROFILE_EAC3_DDP_ATMOS` for the flagged stream: an independent reader of the same bits.
- **AC-4 DSI.** Built bit by bit: 5.1 + 7.1.4, stereo, an EMDF-only plus an object presentation with `pres_bytes` 255 + 16, an unknown presentation version skipped by its length, 5.1.2, every truncation, and dsi version 2.
- **AC-4 descriptor and PMT.** The DVB AC-4 descriptor, and a PMT through the receiver's own parser: 0x06 + AC-4 descriptor, 0x06 + 0x7A, and ATSC 0x87 / 0x81.
- **IEC 61937 framing.** Pa/Pb/Pc/Pd, bsmod in Pc, Pd in bits for AC-3 and bytes for E-AC-3, 1536 and 6144 periods, the 192 kHz link, six one-block frames plus dependent substreams per burst, byte order (the first payload word is 0x0B77), the odd last byte, and refusing an oversized payload.
- **Against FFmpeg's spdif muxer.** The whole AC-3 and E-AC-3 IEC 61937 output was compared byte for byte.
- **Output layout.** Every combination of setting, stream channels and device channels.

What this does not prove:

- **The Atmos streams are simulated**, built from the standards. No real Atmos broadcast recording was available.
- **Passthrough is only checked up to the bytes handed to the device.** The WASAPI, CoreAudio and ALSA device code is syntax-checked and built on CI, but it was not run against a receiver.
- **The 5.1 / 7.1 device output was not listened to.**

## An answer for the user (Discord)

> hey, so in 3.9 OnAir shows when a channel is Dolby Atmos (E-AC-3 JOC on DVB, AC-4 on ATSC 3.0) and plays the 5.1/7.1 bed properly instead of squashing it to stereo, if your output has the speakers
> the actual Atmos objects need Dolby's licensed renderer, which no open-source program can include, and Windows gives apps no way to hand an Atmos stream to the laptop's Dolby Access speakers, so on the laptop itself you get the bed (with Dolby Access's own processing on top)
> if you've got an Atmos receiver or soundbar on HDMI there's a new experimental "pass Dolby audio through" setting that sends the untouched stream so the receiver does the real Atmos, but I couldn't test that on real hardware yet so feedback is welcome

## Files

- `core/include/dect2/audio/dolby_audio.h`, `core/src/audio/dolby_audio.cpp`: headers, beds, AC-4 DSI and descriptor, output layout, IEC 61937 packer
- `core/src/audio/bitstream_out.h`, `bitstream_out.cpp` (WASAPI, ALSA), `bitstream_out_coreaudio.mm`: passthrough devices
- `core/src/audio/audioout*.{cpp,mm,h}`: 2/6/8 channels, `deviceChannels()`, `startBitstream()` / `writeIec()`
- `core/src/tv/player.cpp`, `core/include/dect2/tv/player.h`: detection per track, output choice, passthrough, `reopenAudio()`
- `core/src/atsc3/atsc3_remux.cpp`, `core/src/audio/ac4_dec.cpp`: AC-4 immersive from the `dac4` box
- `rust/onair_ts/src/si.rs`: AC-4 descriptor in the PMT
- `app/panels/tv_dolby_text.h`, `app/panels/tv.cpp`, `app/qo100/qo100_ui.cpp`, `app/iptv/iptv_ui.cpp`, `app/ui/settings_table.h`, `app/ui/settings_window.cpp`, `app/main_start.cpp`, `app/i18n/de.txt`
- Linux: the Settings field "Passthrough output" takes an ALSA name (for example `hdmi:CARD=0,DEV=0`). PulseAudio or PipeWire may hold the device; passthrough needs it free.
