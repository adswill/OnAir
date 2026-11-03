# Recordings folder

One source of truth: `recordingsDir()` (app/state/recdir.h/.cpp, pure part recdir_core.cpp). Pref `recordingsDir` (global, not per receiver);
empty = default `<dataDir>/recordings`. The folder is created on demand and checked for writing; if the chosen one cannot be made or written the
default is used and one warning goes to stderr (Settings > Files shows it too). `recordingsSub("name")` gives sibling folders
(`audio-recordings`, `voice-recordings`, `dab-recordings`, `scan-recordings`): `<dataDir>/name` by default (unchanged), `<chosen>/name` once chosen.
Settings > Files > "Recordings folder": path, free disk space, Choose... (native picker on macOS/Linux; on Windows type the path), Use typed path, Reset to default, Open.
Button: `kit::recordingsButton(id, sub)` / `kit::folderButton(id, dir)` (app/ui/kit/export_ui.cpp), tooltip = the path. Server: onair-server has no use of the folder (no change).
Test: `recdir` (tests/app/test_recdir.cpp).

## Wired (button next to the record controls)
Iridium, TETRAPOL, trunk pool, marine VHF, fast scan, DAB, audio control (Record audio), general receiver.

## Folder changed, no new button (already has one or sits in the Library)
Recording library, Time machine ("Show recording"), TV recorder / scheduler, voice monitor (own "Open recordings folder"), IQ record in the toolbar/diagnostics, passes, automation, TX studio, P25 trunk follow, remote API.

## Skipped
The ~100 mode folders of docs/EXPORT-AUDIT.md write no recordings; modes that only export tables use "Save results..." (a file dialog). Gallery/WeFax/clips keep their own folder buttons.
