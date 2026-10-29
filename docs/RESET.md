# Where OnAir keeps its settings, and how to reset them

OnAir's settings are **not only** in its data folder. On macOS they are in the user defaults of the system, so deleting the OnAir folder
(or reinstalling) leaves them in place. Settings written by an older version that were wrong therefore come back at the next start.

## The easy way

- **Backup panel > Reset all settings ...** (click twice), then close OnAir and open it again. The old settings are kept in a file (below).
- Or start the program once with `--reset-settings` (for example `/Applications/OnAir.app/Contents/MacOS/OnAir --reset-settings`).

Both set the saved settings aside at the next start, before anything is loaded. Recordings, memories, scan lists and the other files in the
data folder stay. The kept copy: macOS `~/Library/Logs/OnAir/settings-crashed.plist`, Linux `~/.config/onair/settings-crashed.conf`,
Windows `%APPDATA%\OnAir\settings-crashed.conf`.

## Every place, macOS

| What | Where |
|---|---|
| Settings (NSUserDefaults) | `~/Library/Preferences/org.onair.dect2.plist` for the installed app; `~/Library/Preferences/<program name>.plist` for a program started outside the bundle (a renamed copy keeps its own) |
| The system's cached copy of the settings (cfprefsd) | in memory; deleting the plist alone can bring the old values back, so use the commands below |
| Data: recordings, memories, scan lists, layouts, plug-ins | `~/Library/Application Support/OnAir/` (or the folder in `DECT2_DATA_DIR`) |
| Caches (map tiles, downloaded data) | `~/Library/Caches/OnAir/` |
| Logs and the kept copy of reset settings | `~/Library/Logs/OnAir/` |
| Window state of the system | `~/Library/Saved Application State/org.onair.dect2.savedState/` (the app does not use it, but it can exist) |

## Clean reset by hand, macOS

Quit OnAir first, then in Terminal:

```
defaults delete org.onair.dect2          # the settings, through the system so the cached copy goes too
killall cfprefsd                         # drop the cached copy
rm -rf ~/Library/Caches/OnAir "~/Library/Saved Application State/org.onair.dect2.savedState"
```

For a complete start from nothing, also move `~/Library/Application Support/OnAir` away (it holds your recordings and memories, keep a copy).
If you ran a copy under another name, repeat `defaults delete <that name>`.

## Linux and Windows

- Linux: settings `~/.config/onair/settings.conf` (or `$XDG_CONFIG_HOME/onair/`), data `~/.local/share/onair/` (or `$XDG_DATA_HOME`).
- Windows: settings `%APPDATA%\OnAir\settings.conf`, data `%LOCALAPPDATA%\OnAir\`.

## The frequency stuck on 1090 MHz (3.1 to 3.3)

Switching from a mode on 1000 to 2100 MHz (ADS-B on 1090 MHz) to almost any other mode kept that frequency, and the mode then remembered it.
Those remembered frequencies live in the settings, which is why a reinstall did not help. From 3.4 the wrong ones (a frequency of 1000 to 2100 MHz
remembered by a mode whose usual band is elsewhere) are dropped once when the settings are read, and the mode comes back on its default.
If something still looks wrong, use the reset above.
