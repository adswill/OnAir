# Tools and notes

<details>
<summary><b>Signal workbench</b></summary>

Takes a recording apart without a radio, in the spirit of Inspectrum, SigDigger and Universal Radio Hacker.

1. **Open** a file (or drop it on the window). Audio files (WAV, MP3, FLAC, OGG, M4A) are treated as a real signal; a stereo file can be read as I/Q ("stereo is I/Q"). IQ files (.cs8 .cu8 .cf32 .ci16 .cu16) are read from disk as they are, so 100 MB and larger files open at once; they need the sample rate (a name like `capture_2Msps.cs8` gives it). SigMF and IQ WAV files describe themselves. "Demo signal" makes a recording to try the tools on.
2. **Look** at the waveform and a large spectrogram. Wheel zooms time, Ctrl + wheel zooms frequency, right-drag pans, a click puts the cursor, a drag selects a time range and a band.
3. **Measure:** peaks, bandwidth (-3 dB, -20 dB, 99 %), carrier offset, SNR, symbol rate, modulation guess, power over time and bursts.
4. **Demodulate:** AM, FM, SSB, CW, ASK/OOK, 2-FSK / 4-FSK, AFSK, BPSK / QPSK / 8PSK, MSK / GMSK, with eye diagram, constellation and the recovered bits; Listen plays the audio.
5. **Read the bits:** invert, differential, NRZI, Manchester, byte reversal and alignment, hex / binary / ASCII, pattern search.
6. **Export** the selection as .wav, .cf32 or SigMF, the audio as .wav, the bits as .txt or .bin, the peaks as CSV.

`wbtool` (built with the tests) does the same on the command line: `wbtool gen out.cf32 --kind qpsk --rate 250000 --baud 12500`, `wbtool analyze file --rate R`, `wbtool demod file --kind fsk2 --baud 9600 --band f0 f1`.
</details>

<details>
<summary><b>Cell tower map (OpenCelliD key)</b></summary>

Cell towers broadcast who they are but not where they stand. The Cells mode can look each decoded LTE cell up in [OpenCelliD](https://opencellid.org), an open, crowd-sourced tower database, and put it on the map:

1. Sign up (free) at https://opencellid.org and confirm your e-mail.
2. Log in and copy your API token from your account page.
3. In OnAir, open Cells → Map and paste it into **OpenCelliD API key**.

Lookups need internet and run at most once a second; only the cell identity (MCC, MNC, TAC, cell id) and your key are sent. Answers are cached on disk, so the map also works offline afterwards. A CSV downloaded from OpenCelliD works too (Load tower file). Tower data © OpenCelliD, CC BY-SA 4.0.
</details>

<details>
<summary><b>Iridium bursts for iridium-toolkit</b></summary>

In the Iridium screen, **Save for iridium-toolkit** writes every demodulated burst as the `RAW:` lines of gr-iridium's `iridium-extractor` (by default `output.bits` in the recordings folder). From the command line: `onair-cli --standard iridium --iridium-bits output.bits --secs 600`. Then run [muccc/iridium-toolkit](https://github.com/muccc/iridium-toolkit):

```
iridium-parser.py output.bits > output.parsed
```

That export is only the bits. OnAir ships no voice codec (the AMBE family is patented), but its Iridium screen also groups voice slots into calls, lists them, and plays and records them through an AMBE-family codec plug-in that you install yourself, see [docs/CODECS.md](CODECS.md). Without the plug-in the calls are listed and silent. The voice path has not been checked against a real Iridium recording yet.
</details>

<details>
<summary><b>TV standards: what has been checked</b></summary>

- **T2-Lite** (1/3 and 2/5 code rates, short FEC frames, 1.7 MHz channels) is decoded like any other DVB-T2 signal and checked on simulated signals; not tried on a real T2-Lite broadcast. A T2-Lite signal sharing a channel with a normal T2 one (in its future-extension frames) is not decoded.
- **ATSC 3.0** is built from the specifications and works on simulated signals (synchronisation, error correction, link layer, ROUTE, HEVC with AAC); details may need fixing against a real broadcast. If you have a recording, please tell us on Discord.
- **ISDB-T** follows ARIB STD-B31 (all modes and guard intervals, up to three layers, TMCC, interleavers, Viterbi, Reed-Solomon) and decodes simulated signals exactly; not checked against a real broadcast. Channel names in the Japanese character set are not converted yet. To try it without a signal, `isdbtgen --out sample.cs8` (built with the tools) makes a recording: open it as an IQ recording, 10 Msps, CS8, with ISDB-T selected.
</details>

**Channel scanner and shared channel list.** The TV channel scanner covers DVB, ATSC, ATSC 3.0, ISDB-T and DTMB. Scan results can be shared through a public, crowd-sourced channel list, picked by country and city; browse it on GitHub: **[adswill/OnAir-channels](https://github.com/adswill/OnAir-channels)**.

**Receive only, other people's signals.** The TETRA, LoRaWAN, LoRa, cell, Bluetooth, Wi-Fi, Zigbee and pager modes read only what is sent in the clear: nothing is decrypted, joined or disturbed. You are responsible for following your country's rules on receiving radio signals.
