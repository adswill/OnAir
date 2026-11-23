# UI audit 4.6 (run 45101733240, 1000x700@1.0 reviewed in full by sheet; 1280x800 picker checked)

Shots: tools/ui_shots.sh now covers 130+ windows (d-*, w-* for every View/Tools/Help window with an Open flag, e00-e17 every Settings page, picker, star card, wizard, setup, trunking) at 1280x800@1.0, 1600x1000@1.25, 1000x700@1.0. No missing frames.

| Window | Result |
|---|---|
| Choose a mode (picker) | DEFECT, fixed: "Only my region" label ran past the dialog. Now measured with CalcTextSize; one row when the search field keeps 220 px, else the checkbox wraps below. Narrow: OK after fix. Tag chips AND (every lit chip must match) by design; empty state now shows a "Clear" button when 2+ chips are lit |
| Controls, Credits, Images, Map, Memory, Data packs, Passes, Recording library, Scanner | OK |
| Settings (all 18 pages), Setup wizard, Tour/wizard, Star card, View menu | OK at coarse sheet scale |
| Trunking, Antenna designer, Automations, Backup, Checkup, Datasets, Direction finder, Doctor, EchoLink | OK |
| Data feeds, Frequencies, Glossary, Hamlib add, Hardware, Search heard, Insights, Integrations, Lessons, Mode Builder, Modes shown, Messages | OK |
| Passive radar, Plugins, Protocol lab, Recordings, RF fingerprint, Interference hunter, Rotator, SatNOGS, Scheduler, Signal Lab, My station | OK (rfi / sweep: drag fields with embedded labels, dense but not clipped) |
| Station health, LAN swarm, Sweep, TX studio, Video generator, Voice codecs, Voice keys, Watchlists, Everything in view, Wireshark | OK |
| Light theme | OK |

Not covered: Support dialog, export dialogs (no dev open flag); sheets are coarse, fine-print overlaps below ~10 px may be missed. 1280 and 1600 sets only spot-checked.
