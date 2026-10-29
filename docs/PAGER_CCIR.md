# CCIR paging in OnAir

**CCIR Radiopaging Code No. 1 (RPC1) is POCSAG.** The CCIR (now ITU-R) adopted the Post Office Code Standardisation Advisory Group
code as Radiopaging Code No. 1 in Recommendation 584 (ITU-R M.584). There is no separate CCIR decoder to add:

| Item | RPC1 / POCSAG |
|---|---|
| Speeds | 512, 1200 and 2400 baud, FSK, 4.5 kHz deviation |
| Code word | 32 bits: 21 data bits, 10 BCH(31,21) check bits, 1 even-parity bit |
| Preamble | alternating 1010... for at least 576 bits |
| Batch | frame sync word 0x7CD215D8, then 8 frames of 2 code words (16 code words) |
| Idle word | 0x7A89C197 |
| Address word | flag 0, 18 address bits + 2 function bits; the frame number (0-7) supplies the last 3 bits of the 21-bit RIC |
| Message word | flag 1, 20 message bits; numeric (4-bit BCD) or alphanumeric (7-bit ASCII) |

OnAir decodes this in Pagers > POCSAG (512, 1200 and 2400 baud boxes). A transmission that the requester calls "CCIR" and that shares the
pager bands is therefore already received; if it does not decode, check the channel width (12.5 / 25 kHz), the frequency offset and the
"Message type" setting (some networks send text with function 0).

## German umlauts (character set)

Alphanumeric POCSAG text is 7-bit. German networks use the national variant DIN 66003 (ISO 646-DE) and send the umlauts in the places of
`[ \ ] { | } ~`: `[` = Ä, `\` = Ö, `]` = Ü, `{` = ä, `|` = ö, `}` = ü, `~` = ß. In Pagers > POCSAG the "Characters" box picks the set:
Automatic (German when your setup location is Germany, Austria or Switzerland, plain elsewhere), Plain 7-bit, or German. The choice applies
to what is shown, copied, saved, filtered and sent to automations; the raw characters are kept, so changing the box re-reads old pages.
`@` is left as `@` (DIN 66003 puts the section sign there, but networks keep `@` for e-mail addresses). Numeric pages and FLEX are not changed.

Other things called "CCIR" that are NOT paging code No. 1:

- CCIR 5-tone sequential selective calling (the "CCIR" tone set of ZVEI / EEA / EIA / DZVEI). That is in the Tones mode.
- The Golay Sequential Code (GSC) and other codes in the same ITU-R M.584 annex are different, older paging codes. If a signal does not
  decode as POCSAG, send a short IQ recording and the network name so the code can be identified.

So there is no "CCIR mode" checkbox: it would only repeat the POCSAG one. The POCSAG check box has a tooltip that says this.
