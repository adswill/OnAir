# Airband: one recording per transmission (3.9)

Round-4 item 10 (scratchpad r4-per-mode list; RTLSDR-Airband, SDRTrunk). Branch feat39-r4.

- **Airband > Recordings > Save each transmission** (off by default): every squelch opening of every channel is saved as a short 8 kHz WAV in `<recordings>/airband/`, for example `airband-TOWER-118.100-20261110T221530Z.wav` (the label or channel name, the frequency, the UTC start).
- A clip ends after 1 s of closed squelch, so a short pause inside one call does not split it. The receiver's hang time is part of the clip, and the trailing silence is cut.
- Openings shorter than 0.4 s (squelch clicks) are not saved. A stuck carrier is cut into clips of at most 10 minutes.
- The clips appear in the same **Recent voice clips** list the voice modes use: Play inside OnAir, star, save as, delete, open folder. The retention settings there also handle "delete after N days".
- **30 s** next to **Listen** on each channel saves the last 30 s of transmissions on that channel (without the gaps between them) as a clip. It works whether or not saving each transmission is on. The view then switches to Recordings.
- The recorder takes the same per-channel audio after the squelch as the transcription tap, through its own hook (`AirbandReceiver::setClipWriter`), so both can run at once.

## Tests
`tests/airband/test_airband_clips.cpp`:
- file names
- the writer alone: clip lengths, a pause inside one call, a 0.2 s click dropped, a readable WAV, the replay (calls and their tail, no gaps), switched off, a stuck carrier cut at 600 s
- the real receiver on two generated channels (voice-like and a tone, 6 dB apart) with a 2 kHz tuning error at 25 dB SNR: one clip per transmission, named with the channel and the frequency

The test runs in CI. Simulated signals only. The UI was syntax-checked only.
