# Multi-VFO: several channels on one capture

Request: "is there a way to multiplex a mode? Two pager signals at the same time, or two FM stations at once" (CubicSDR lets you put several
demodulators on the waterfall; they play to the same sound card when above their squelch).

## Use

* Spectrum/waterfall popup (the display button) > **Channels > Add channel**, then click on the waterfall. **Alt+click** on the spectrum or
  waterfall adds one directly. **Channel list** opens the list window.
* A new channel starts in a sensible mode for its frequency (FM broadcast: WFM, air band: AM, below 30 MHz: SSB, otherwise NFM; when the main mode is
  pagers or APRS the new channel is a pager or APRS decoder).
* Each row: on/off, frequency, mode (NFM, AM, USB, LSB, WFM, Pager, APRS), bandwidth (blank = the mode's default; any value), squelch (dBFS, "open"
  = always), volume, pan, mute, level and the state of a decoder. The band of every channel is drawn on the spectrum and waterfall (drag the
  centre line to move it, double click opens the list).
* The sound of the channels above their squelch is mixed (volume, pan, mute per channel) and played beside the main mode's own sound.
* A decoder channel (Pager = POCSAG 512/1200/2400 and FLEX, APRS = AX.25 1200/9600) runs the normal receiver on its own channel; its pages or frames
  and counters show in the channel's row (`Engine::vfoPager()` / `vfoPacket()` give the full telemetry).
* Default off: with no rows nothing runs and the mode's own receiver is untouched. Rows are saved in the preferences (`vfoRows`).
* No limits: any number of channels, any bandwidth. When the CPU cannot keep up the list shows a warning (load above 85 %); a channel outside the
  capture or narrower than a decoder needs shows "!" with the reason; nothing is refused.

## Design

`core/include/dect2/engine/vfo.h` / `core/src/engine/vfo.cpp`: `VfoBank`. Per channel: `ChannelFilter` (mix to zero, exact resample to 48 kHz or
a multiple of it, 192 ksps for decoders as in the whole-span banks, 500 ksps for WFM through `FmReceiver`), a band filter, the demodulator, the squelch
(2 dB hysteresis, 0.25 s hang) and the mixer. `Engine::feedReceiver()` feeds the bank only when channels exist. `app/vfo/vfo_ui.cpp` has the screens;
the rows are given to the engine every frame as offsets from `captureCentreHz()`, so a retune keeps channels at their absolute frequencies.

Not shared with the per-listener virtual receivers of the web hub (`server/web/web_dsp.cpp`): those cut channels from one shared FFT, this runs
time-domain channels on the radio's own stream. They could be unified later.

## Test

`tests/engine/test_vfo.cpp` (`ctest -R vfo`): one 2 Msps capture with two FM carriers (1000 and 1700 Hz tones, panned left/right), two pager carriers
(POCSAG 512 and 1200, simultaneous), an APRS carrier and an empty channel with a squelch; both stations are heard, the muted one drops out live, every
page of both pagers arrives with its text, APRS frames decode, the empty channel stays closed, and an out-of-range channel is kept with a warning.
