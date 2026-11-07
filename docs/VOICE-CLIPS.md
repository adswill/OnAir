# Recent voice clips

Every voice mode that saves calls as WAV files (Iridium, DMR, P25, NXDN, dPMR, D-STAR, Yaesu, TETRA, M17) shows the same list.

- **Iridium**: Calls view, under the call table.
- **DMR, P25, NXDN, dPMR, D-STAR, Yaesu, TETRA, M17**: Voice monitor window, **Clips** tab.

Each row has the time, the source (frequency and direction, or talkgroup and ID), the length, and:

| Button | What it does |
|---|---|
| Play / Stop | Plays the clip inside OnAir, on the normal volume and mute of the app |
| Keep (tick) | Stars the clip: it is never deleted by the limits below |
| Save as... | Copies the WAV to a place you choose |
| Show | Opens the folder |
| Delete | Deletes the file and the entry |

## Keeping the folder small

By default OnAir keeps the newest **20** clips per mode and deletes older ones (file and list entry) as soon as a new clip arrives.
Under **Advanced** (applies to every voice mode, saved in your settings):

- **Keep the last N clips**: 1 to 1000; 0 keeps all.
- **Delete clips older than N days**: 0 = never.
- **Total size cap in MB**: the newest clips that fit are kept; 0 = no cap.

Starred clips do not count towards any limit and are never deleted automatically. Clips from earlier runs in the same folder are included.
Only `.wav` files in the mode's own recording folder are touched (Iridium: `iridium-call-*.wav` in the recordings folder).

## Iridium: silent WAV files

OnAir ships no Iridium vocoder (see CODECS.md). A call is saved only when the plug-in produced sound.

- No plug-in installed: the call is listed as **codec needed**, a message says so, and no file is written.
- Plug-in loaded but it returns only zeros (loudest sample below 64 of 32768): the call is **silent audio**, a message says so, and no file is written.
  This usually means the plug-in does not match the frame layout it receives.
- The file could not be written: a message says so, and a partial file is removed.
