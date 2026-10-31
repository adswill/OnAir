# SSB conversations (View > Conversations)

Finds single-sideband voice (USB and LSB) on the captured span by itself and organises it into conversations you can just listen to. Code: `core/src/ssbconv` (finder, voice analysis, conversation book, synthetic signals), `app/ssbconv` (the window), tests `tests/ssbconv`.

## What the user sees

- One switch, "Find SSB conversations" (on by default while the window is open and a radio runs). Nothing else has to be set.
- A list, one row per conversation: **ACTIVE now** badge (voice in the last 4 s) or "quiet" / "ended 3m 10s ago", frequency, sideband, band and service (amateur, maritime SSB, aviation HF, CB, utility), estimated signal in S-units with a rising / falling mark, start, length, overs, voices, kind (single call, QSO, rag chew, net), callsigns heard (when a transcript exists).
- Filters: band, USB / LSB, minimum S, active only, hide ended, favourites, hide short, show ignored. Sort by activity, strength, newest or frequency.
- **Listen**: puts a USB / LSB channel on the conversation. Inside the captured span the radio is **not retuned** (it is a Multi-VFO channel, docs/MULTI-VFO.md); outside it the radio moves (centre 25 kHz below the signal on wide radios) and the note says so.
- **Auto-listen to the best active conversation** with **Hold** (stay while it lasts) and **Skip** (next best, not this one again until it ends). When the conversation goes quiet it waits 2.5 s, fades out over 0.6 s and fades the next one in.
- **Rec** per conversation (WAV plus a JSON note in `<data folder>/ssb-conversations`), "Record every new conversation" in Settings; both off by default. **Ignore** a frequency, star a favourite.
- Timeline tab: one row per frequency, a bar for every stretch of voice over the last 15 minutes to 24 hours. Settings tab: gap that ends a conversation (default 30 s), pause that starts a new over (2 s), same-frequency tolerance (+-100 Hz), export CSV / JSON, band scanner for narrow radios.
- Demo button: the same panel on synthetic signals (nothing is received); `--ssb-conversations demo` opens it at start (screenshots).
- Events for Automations / Integrations / Watchlists: `signal.ssb_conversation_new` and `signal.ssb_conversation_ended` (fields id, freq_hz, sideband, band, service, overs, speakers, snr_db, kind, talk_s).

## How it finds them (core/src/ssbconv)

1. **Spectrum**: Welch spectrum of every 0.5 s window of the capture (bin <= 45 Hz where the rate allows, up to 12 FFTs per window), DC spike removed, smoothed over 250 Hz. A decayed max-hold (0.6 dB per window) gives the shape of a station over a few seconds, because speech has gaps in every window. Bands 1.2 - 4.4 kHz wide that stand 3.5 dB over the median floor are candidates (an AM broadcast, a wide data signal or a single tone is outside). A piece of a band a track already covers is not a new station.
2. **Sideband**: spectral tilt (more energy in the end next to the suppressed carrier, because speech falls 6-12 dB per octave), weighted with a small vote from the band convention (LSB below 10 MHz, USB above; 60 m, maritime, aviation and utility USB). The tilt wins: USB on 40 m and LSB on 20 m are reported as they are. The decision is averaged over the track (and the audio analysis gives a mirrored sideband a low score).
3. **Channel**: for each candidate a CIC3 (exact integer arithmetic) to about 48 kHz, the suppressed carrier mixed to 0 Hz, a FIR to 3.2 kHz, decimation to about 8 kHz. Dial = carrier-side edge -/+ 160 Hz (accuracy about +-100 Hz; it follows drift because the tracks follow the edge).
4. **Voice analysis** (10 ms frames): depth of the energy among the active frames (keyed data is flat while it is on), syllabic modulation at 2.5 - 8 Hz, pitch from the complex autocorrelation (a voice has a peak at the pitch period, a flat tone has the same correlation at every lag), spectral lines (a carrier or an FSK tone is 25 dB over the next one), share of the energy at 250 - 2900 Hz, tilt of the audio (the mirrored sideband scores low). A track needs 6 good windows of its last 12 to be confirmed. Voice activity is energy over the noise floor with a 0.35 s hang time.
5. **Digital decoys**: the score above already rejects ALE 8-FSK, RTTY, PSK31, FT8, a multi-tone modem, band noise and a carrier (tests). `ScannerOptions::digitalVeto` is the hook for a second opinion from `sigid::identify()` (core/src/engine/sig_ident.cpp): it is called for tracks that are not confirmed yet. The window wires it to the identifier and lets a clear (75 % or more) HF data / utility result (ALE, RTTY, FT8, FreeDV, ROS ...) veto the band; the unit tests use a fake veto, and the identifier path was only compile-checked here.
6. **Conversations** (`ConversationBook`): a voice detection joins the conversation of its track, else the nearest one within the tolerance, else starts one. An **over** is continuous voice; a pause of 2 s ends it, and so does a change of pitch for three windows in a row. **Voices** are clustered by median pitch and offset. Kinds: net (3+ voices, 6+ overs, short overs), rag chew (long overs or 10+ minutes), QSO (2+ overs), single call. A conversation ends after the gap (30 s); a blip with no over is not listed. Signal and trend come from the estimated SNR (S9 = 48 dB over the noise floor of the capture; this is **not** calibrated to microvolts).
7. **Callsigns**: `ConversationBook::addClue(id, text)` reads amateur callsigns from text ("W1ABC", "whiskey one alpha bravo charlie"). The window does not feed it yet: the transcription engine (core/src/transcribe) works per channel for airband and marine, and connecting it to a conversation's audio is the open hook.

## Band scanner (narrow radios)

`SweepPlanner` steps through the HF amateur bands and the maritime / aviation blocks (80% of the span per step, 6 s dwell, up to 40 s while a conversation is active) and the window retunes the radio; the detector is the same. Not tested against a real radio.

## Tests (`tests/ssbconv`, ctest `ssbconv`)

Synthetic speech-like SSB (harmonics on a vibrato pitch with formants, syllables, word gaps, fricative tones) at 96 kHz and 1.92 MHz, with noise, fading, drift and offsets: the voice analyzer on speech and six decoys; the conversation state machine without any GUI (alternating overs, drift, end, net, filters, mute, favourite, export, timeline); auto-listen (skip, hold, switch); the sweep plan; the clip files; the scanner on a scene with two stations alternating on one frequency, an LSB station that drifts against the band convention, ALE, a multi-tone modem, RTTY, PSK31, FT8, band noise, a carrier and an AM station next to them (exactly two conversations, right sidebands, no decoy), noise only (nothing), USB on 40 m, an impairment sweep (offsets near both span edges and close to DC, both sidebands, three pitches, three levels), and the Demo scene.

## Honest limits

- Only synthetic signals were available: no real HF recording was used. The speech is a generator with the properties the detector looks at, not a voice. Real band noise, QRM, adjacent SSB at 3 kHz spacing, selective fading, AGC pumping and different transmit filters are untested.
- Sensitivity: stations about 18 dB or more over the noise in their own bandwidth are found; at about 14-16 dB with 6 dB of fading 3 of 4 test stations were found. Confirmation takes about 3-5 s of speech, so the start of a conversation is back-dated but its first seconds are not recorded.
- Two SSB stations closer than the width of one (about 3 kHz apart) merge into one band and are rejected; stations inside +-150 Hz of the DC spike are affected by the spike removal.
- A pair of speakers with similar pitch and no pause between them counts as one voice; the voice count is a guess for nets.
- The dial estimate is +-100 Hz: enough to recognise the voice, not to be exact. Fine-tune with the VFO.
- One sample tap serves the Conversations window; the Reception Doctor or the frequency calibration take it while they capture, then it is set again within 3 s.
