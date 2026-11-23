# Airband Auto: a short guide

The Airband mode listens to aircraft radio (118 - 137 MHz, and military UHF 225 - 400 MHz) in AM. With **Auto** on (the default) you do not need a channel list or an airport: OnAir finds the stations by itself.

## What Auto does

- It watches the whole part of the band your radio can see. When a carrier switches on (someone presses the microphone) on a channel of the 25 kHz or 8.33 kHz raster, OnAir opens it and plays it.
- A channel that stays quiet for 30 seconds is closed again (120 seconds if it was busy). At most 24 auto channels are open at a time.
- A carrier that stays on for 90 seconds or more (ATIS, VOLMET) is marked **continuous** and is left out of the scan, so it does not hold the sound all the time.
- A weak carrier right next to a much stronger one is treated as its splatter and is not opened.
- What it heard is remembered, and busy channels are checked more often the next time.
- Channels you added yourself to the list still work and come first.

## What to set

1. Start your radio and pick the Airband mode. Leave **Auto** ticked.
2. Use a sample rate that takes in as much of the band as possible (2 Msps or more; 118 - 137 MHz is 19 MHz wide). Click the dial to move it, or raise the sample rate in Tuner.
3. If your radio cannot see the whole band at once, keep **Hop across the band** ticked: OnAir moves between parts of the band, stays while someone is talking, and comes back often to busy parts and rarely to dead ones. Hopping runs only while the channel list is empty.
4. **Squelch** (default 6 dB) is the carrier-to-noise a channel needs to open. Lower it to hear weaker stations (more false openings), raise it if the noise opens channels.
5. **Scan** plays one channel at a time instead of mixing every open channel. **Hold** stays on the channel it stopped on.
6. A good antenna matters most. Airband is line of sight: you hear aircraft and towers within about 200 km when they are high, a few tens of km when they are low.

## The Listen button

- In the **seen carriers** list, **Listen** plays that carrier at once, without adding it to your list. It stays until it has been heard and has closed. **Add** puts it in your list.
- In your channel list, **Listen** plays only that channel (solo) and silences the others. Press **Stop** to hear every open channel again.
- Clicking on the spectrum plays the nearest channel of the raster.

## Guard channels

121.500 MHz (civil emergency) and 243.000 MHz (military emergency) are always kept open when your radio can receive them, and they have priority over everything else: when one opens, the others are pushed down. They are usually silent, so a quiet guard channel is normal.

## Tips

- Nothing heard? Airband is quiet at night and away from airports. Check that your radio covers 118 - 137 MHz and that the antenna is outside or at a window.
- Voice is AM and your radio's tuning can be off by a few kHz: the **Radio off by** value shows the measured error, and OnAir corrects each channel for it.
- Auto opens a channel on the carrier it sees. If the frequency shown looks wrong by a full MHz, tell us your radio, sample rate and a screenshot: on the 25 kHz raster the names are the dialled channel names (for example 126.200), so a one MHz difference is a bug to report.
- Save each transmission (tick it in the panel) writes one short WAV per transmission, named with the channel and the UTC time.
- With an airport picked (Airport box) the list shows the airport's own channels (tower, ground, ATIS) instead of what Auto found. Both work together.
