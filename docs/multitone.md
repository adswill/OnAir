# Multitone paging (detector, with measured framing)

Multitone Electronics (UK) pagers, Mk IV to VII (hospitals, industrial sites; 25 to 470 MHz, narrowband FM). The format is proprietary and not
published: sigidwiki.com/wiki/Multitone_Paging says FFSK "similar to POCSAG 1200" but with its own headers, readable only by Multitone pagers, and no
specification, tone table or sample of the digital format is public. So this mode is a detector, not a decoder, and every report is labelled unverified.
No message text is produced, nothing is decrypted or transmitted. (If you have the format description, send it: the receiver has the hooks.)

## What is reported per transmission

The carrier (up to carrier gone), the FM deviation, the spectral lines and steady tones of the audio (a tone list in order of arrival with durations),
and, for two-level FSK or audio FFSK: the measured baud rate, the bit stream in hex, how many 32-bit words pass the POCSAG BCH(31,21) check, whether
the POCSAG sync word is in it, the 0x605A word count, and (new in 3.6) the **framing measured from the bit stream**.

## Framing measured on the owner's recording (3.6)

Recording: `Multitone_2015-12-31T01-37-17Z_27599.8kHz.mp3` (FM audio of 27.5998 MHz, 8 kHz, 28 s). A cycle of 12 s repeats: an FFSK burst of 2.9 s,
the squelch noise of the carrier going away (1.4 s), then a tone sequence of about 3.4 s (a steady 470 Hz tone for 0.5 s, then a 234 Hz tone with its
harmonics for about 1 s, then 234 / 250 Hz steps), noise, and the next cycle.

The FFSK burst: the audio tones are 292.8 Hz and 585.6 Hz (f and 2f; the existing slicer reads a 585.6 Hz cycle as 1 and a 292.8 Hz half cycle as 0), so the symbol
rate is **585.6 baud** (1682 bits in 2.9 s). The stream is made of **48-bit frames**: a 24-bit marker `000000101101001001110110` (26 times, spaced exactly 48 bits)
followed by a 24-bit word (the 16-bit word 0x605A of the older recordings is in the stream 12 times as well). The 26 words after the marker are all different. In
the first ten the structure is a first byte (0x76 or 0x77) and then a value that is sent twice (the two bytes are equal within one bit: 54 / 55, DC / DD, DC / DC,
54 / 54, 00 / 01, 88 / 89, AA / AB, 22 / 23, 76 / 77 ...), in the next sixteen the first byte is 0xFE or 0xFF and the two last bytes differ. That is all that is
measured: how the words map to pager addresses, tones or text is not known and is not guessed. The numbers are tested on an 8.2 s excerpt of the recording
(`tests/data/multitone/`, `test_multitone_real`).

The same burst was first read at 293 baud (one cycle of 293 Hz or two cycles of 586 Hz per bit): there a 12-bit marker comes back every 24 bits and each word is
repeated once. That is the same stream read with half the resolution; the 586 baud reading, with distinct words, is the one the receiver reports.

Plan B for a real decoder: the format description (word table and the address field) or a recording with a known message sent to a known pager.

## Tests

`tests/multitone`: the detector on the generator's signals (`test_multitone_rx`, `test_multitone_engine`) and the owner's recording (`test_multitone_real`).
