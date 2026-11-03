# Plugins

Plugins let other people add decoders ("modes") and actions to OnAir without rebuilding it. A plugin is a shared library plus a `manifest.json`. OnAir
loads it, gives it IQ samples (or automation events), and draws whatever the plugin reports with one generic screen, so a plugin author writes no UI
code. The SDK is in `sdk/` (header, CMake helper, three C++ examples, a Rust crate); the host is in `core/src/plugin/`.

- [Concepts](#concepts)
- [Writing a plugin](#writing-a-plugin)
- [manifest.json reference](#manifestjson-reference)
- [Host API reference](#host-api-reference)
- [Plugin functions](#plugin-functions)
- [Security model](#security-model)
- [Out-of-process plugins](#out-of-process-plugins)
- [In the program](#in-the-program)
- [Distributing a plugin](#distributing-a-plugin)
- [Licences](#licences)
- [Limits](#limits)

## Concepts

| Kind | What it is | Gets |
| --- | --- | --- |
| `mode` | a decoder: appears in the mode list under **PLUGINS** | IQ blocks of the tuned channel |
| `source` | like `mode`, meant for plugins that mostly analyse the stream | IQ blocks |
| `action` | reacts to what other modes decode (like an automation rule, but in code) | automation events |
| `panel-lite` | only a manifest and settings, no code | nothing |

Each running plugin mode is one *instance*. OnAir calls `create()` when you open the mode and `destroy()` when you leave it. All calls into the
plugin come from one worker thread of its own, so a plugin needs no locking of its own for those calls. A plugin can never block the radio: OnAir
queues IQ for it (8 MiB at most) and drops blocks when it is too slow (the count is shown on the plugin's screen).

What a plugin reports goes into a bounded store that the generic Plugin screen draws:

- **tables** of typed rows (text, integer, float, frequency, time, flag), newest first
- **status lines** with a lamp (off, ok, warning, bad)
- a **log** of text lines
- **plots** of a float array: spectrum (dB against frequency), time series, or constellation (I/Q pairs)
- **images** (RGBA or PNG)
- **map points**

It can also **emit automation events** (`plugin.<type>`, usable in Automations rules), **play audio**, and declare **settings** (slider, checkbox,
combo, text) that OnAir draws and saves.

## Writing a plugin

1. Copy `sdk/template/` (a working IQ plugin), rename `my-plugin` in the folder, `manifest.json`, `CMakeLists.txt` and the id in `plugin.cpp`.
2. `cmake -S . -B build -DONAIR_SDK_DIR=/path/to/OnAir/sdk && cmake --build build`
3. Copy `build/plugins/my-plugin/` into OnAir's plugins folder (**View > Plugins > Open folder**), press **Refresh**, enable it, open it from the
   mode list.

The examples in `sdk/examples/` are built with the repository (`build/plugins/<id>/`) and shipped next to the program as bundled examples:

- `tone-detector`: finds the strongest tone in the channel (offset in Hz, level, spectrum plot). About 100 lines.
- `burst-counter`: counts bursts above a threshold over the noise floor, with start time, length and peak; emits `plugin.burst`. Has two settings.
- `log-to-file`: an action plugin that appends automation events to `events.log` in its data folder (needs the *filesystem* permission).

Rust: `sdk/rust/onair-plugin` is a dependency-free crate with the C ABI written out by hand, a safe `Host` wrapper, a `Plugin` trait and the
`export_plugin!` macro; `sdk/rust/examples/level-meter` is a complete plugin (`cargo build --release`, then copy the library and manifest).
Other languages work the same way: export `onair_plugin_entry` with the C ABI.

## manifest.json reference

```json
{
  "id": "burst-counter",                 // required: a-z 0-9 - _ . (starts with a letter or digit), at most 64; also the folder name
  "name": "Burst counter",               // required
  "version": "1.0.0",                    // required: dotted numbers, optional suffix
  "author": "OnAir contributors",        // required
  "license": "CC0-1.0",                  // required: an SPDX id or a short text
  "homepage": "https://example.org",     // optional, http(s) only
  "description": "What it does.",        // optional, at most 2000 characters
  "kind": "mode",                        // required: mode | action | source | panel-lite
  "abi_version": 1,                      // required: must equal the ABI OnAir speaks
  "minimum_onair": "0.2.7",              // optional
  "library": {"macos": "x.dylib", "linux": "x.so", "windows": "x.dll"},   // file names inside the folder (no paths)
  "permissions": ["filesystem"],         // network | filesystem | audio-out | record | spawn-process
  "modes": [                             // required for mode and source (at most 16)
    {"id": "bursts", "name": "Burst counter", "group": "Plugins",
     "default_frequency_hz": 433920000, "bandwidth_hz": 500000, "sample_rates": [1000000, 2000000]}
  ],
  "settings": [                          // optional, at most 64
    {"key": "threshold_db", "label": "Threshold (dB)", "type": "slider", "min": 3, "max": 40, "default": 10},
    {"key": "on", "label": "On", "type": "checkbox", "default": true},
    {"key": "kind", "label": "Kind", "type": "combo", "options": ["a", "b"], "default": 0},
    {"key": "prefix", "label": "Prefix", "type": "text", "default": ""}
  ]
}
```

(Comments are not valid JSON; they are only here.) The manifest is read with a bounded parser: at most 64 KiB and 32 levels deep. Anything
malformed, any id or library name that could point outside the plugin's folder, an unknown permission or kind, or a different ABI version makes
the plugin show up in the manager with the reason and refuses to run it.

Settings are stored per plugin id. A combo's value is the index of the chosen option. The plugin reads values with `get_setting_*` and is told
about changes through `setting_changed()`.

## Host API reference

`sdk/include/onair_plugin.h` is the contract; every function of `OnAirHostApi` takes the table's `ctx` first and returns 0 for success, a negative
number when refused (bad argument, limit reached, permission missing).

| Function | Purpose |
| --- | --- |
| `log(level, msg)` | diagnostics, shown in the plugin's log in the manager |
| `get_setting_text / _number / _flag` | current value of a declared setting (the default when unset) |
| `request_iq(centre_hz, sample_rate)` | ask for a centre frequency (0 = keep). The frequency is applied; the real values arrive in `OnAirIqMeta` |
| `table_define / table_row / table_clear` | tables; rows beyond 2000 push out the oldest |
| `status(key, text, lamp)` | a status line (at most 64) |
| `result_log(line)` | a line in the results log (500 kept) |
| `plot(id, title, kind, data, n, x0, dx)` | up to 8 plots of up to 65536 floats each |
| `image(id, format, w, h, data, size)` | up to 4 images, 4 MiB, 4096 pixels per side |
| `map_point(id, label, lat, lon)` | up to 2000 points |
| `emit_event(type, n, fields)` | automation event `plugin.<type>` (100 a second per plugin) |
| `audio_out(pcm, n, rate)` | mono float PCM, 1 kHz to 400 kHz; played on the default output device (resampled to 48 kHz) at the app's volume and mute; needs `audio-out` |
| `data_dir(out, cap)` | the plugin's own data folder; needs `filesystem` |

Text longer than the limits is cut, non-finite numbers are replaced, control characters become spaces. Memory passed to any call is copied.

## Plugin functions

`onair_plugin_entry(host_abi)` returns an `OnAirPluginVTable`: `create(host, mode_id)` (return NULL to refuse), `destroy`, `configure(centre, rate)`,
`process_iq(iq, n, meta)` (interleaved float I, Q), `on_event(event)`, `tick(now)`, `setting_changed(key)`. Optional entries are NULL; `caps`
says which are implemented (`ONAIR_CAP_IQ_SINK` is required for modes, `ONAIR_CAP_EVENTS` for actions). Structs start with `struct_size`; read only
fields that fit inside it. Do not let C++ exceptions or Rust panics leave a callback.

## Security model

Be clear about what this protects and what it does not.

- **Stability.** A plugin runs on its own thread. A call that does not return within 2 seconds (the watchdog) switches the plugin off: OnAir stops
  feeding it and stops waiting for it. A thread stuck in foreign code cannot be killed safely, so an in-process plugin that hangs is *abandoned*
  (its thread and library stay until the program exits). A plugin that throws, or whose `create` returns NULL, is reported and switched off. The
  manager switches off a failed plugin and logs why.
- **Crashes.** A crash (segmentation fault) of an in-process plugin takes the program down. While plugin code runs OnAir keeps a marker file in
  the plugins folder; if the marker is found at the next start, OnAir starts in **safe mode**: no plugin is loaded, and the plugin that was running is
  left disabled. **View > Plugins > Try plugins again** (or the Safe mode box) leaves safe mode. Plugins running in a helper process leave no marker,
  since they cannot take the program down.
- **Permissions.** A plugin declares what it wants; the first time it is turned on, OnAir shows the list and you allow or refuse each. OnAir
  hands out what the permissions control (`data_dir` and `audio_out` return an error without them). It does **not** sandbox the operating system for an
  in-process plugin: the library is native code with the program's own rights and can open files or sockets itself. Treat installing a plugin like
  installing a program: install only ones you trust, read what they ask for, and prefer the helper process for ones you do not fully trust. A
  sandboxed helper (macOS sandbox profile, Linux seccomp) is a planned hardening step, not part of this version.
- **Packages.** `.onairplugin` files are zip archives. OnAir reads them with a bounded reader (64 entries, 128 MiB per file, 256 MiB in all, CRC-32
  checked, no encryption, no zip64), refuses names with `..`, absolute paths, backslashes or drive letters, validates `manifest.json` and that a library
  for this platform is present, and unpacks into a temporary folder before replacing the plugin. Nothing is downloaded by OnAir: there is no store and no
  auto-update; **Community plugins** only opens the wiki page in your browser.
- **Encrypted traffic** is never decoded by OnAir, and plugins should not do it either. Patented codecs are not bundled; a plugin that needs one
  must have the user install it, as OnAir's own voice modes do.

## Out-of-process plugins

Tick **Run in a helper process** for a plugin in the manager (macOS and Linux; Windows is not supported yet). OnAir then starts
`onair-plugin-host --plugin-host <library>` (shipped next to the program) and talks to it over pipes: the helper reads length-prefixed messages on
fd 0 and writes results on fd 3 (its stdout and stderr are discarded). Messages carry the plugin id, mode, permissions and settings (`Init`), then
`Configure`, `Iq`, `Event`, `Tick`, `Setting`, `Quit` down, and `Hello`, `Ack`, results, `Event`, `Audio`, `RequestIq`, `Log`, `Fatal` up (`core/include/dect2/plugin/results.h`,
`Wire`). Every call is acknowledged; a call not acknowledged within the watchdog time gets the helper killed (SIGKILL), and a crash ends only the
helper. IQ is copied through the pipe (about 8 bytes per sample), so very high sample rates cost CPU; the in-process path is the default.

## In the program

- **View > Plugins** (and the **Plugins** row in the PLUGINS group of the mode list) is the manager: the installed and bundled plugins with
  version, author, licence, kind and permissions; enable/disable; the first-use permission prompt; settings; each plugin's log; **Install from
  file...** (a `.onairplugin`); **Open folder**; **Remove**; **Safe mode**; **Community plugins**.
- Plugin modes appear in the mode list under **PLUGINS** with a stable number (family 1000 and up, stored in `plugins/state.json`: assigned in order of
  first appearance, never reused, kept when a plugin is removed and reinstalled). A plugin's settings are saved per plugin id.
- Plugin modes run on receiver 1. The plugins folder is `plugins/` in OnAir's data folder. The bundled examples and `onair-plugin-host` are looked for next to the program
  (`plugins/`, `../Resources/plugins/` in an app bundle, `../lib/onair/plugins/` for the Linux packages, which install them; the macOS and Windows
  packaging scripts do not copy them yet, so there only the build folder's `plugins/` has them).

## Distributing a plugin

Zip the plugin's files (`manifest.json` and the library for each platform you support, plus anything else the library needs, at most four folder
levels) as `<id>-<version>.onairplugin`, either at the top of the zip or inside one folder. One package can hold the libraries of all three
platforms; the manifest's `library` object names each. Sign nothing and promise nothing about trust: users decide whom to trust. Put the source
and the licence where people can read them.

## Licences

OnAir is GPL-3.0-or-later. This is **not legal advice**; read the licences yourself.

- **The ABI header** (`sdk/include/onair_plugin.h`) is dedicated to the public domain (CC0-1.0) and the examples and the Rust crate are CC0, so
  they can be copied into a plugin of any licence.
- A plugin is a **separate work** when it only uses the stable C ABI of that header: it does not include OnAir's other headers, link OnAir's code
  or use OnAir internals. Then it is your choice of licence (including closed source), and OnAir does not make it a derivative work by loading it
  through the C ABI. That is the intent of the ABI.
- A plugin that is loaded into OnAir's process **and** uses OnAir's own headers or code (`core/include/dect2/...`, `app/...`) is, in all
  likelihood, a derivative work of OnAir under GPL-3.0: distribute it under GPL-3.0-or-later (or a compatible licence) with its source.
  Copying OnAir source into a plugin is the same.
- To keep a plugin clearly separate, use only the header in `sdk/include/`, or run the plugin in the helper process.
- Plugin authors keep the notices of any third-party code they include, as OnAir does (`THIRD_PARTY.md`).

## Limits

| What | Limit |
| --- | --- |
| manifest | 64 KiB, depth 32, 16 modes, 64 settings, 64 combo options |
| IQ queue per plugin | 8 MiB (in-process) |
| watchdog | 2 s per call; start-up 3.5 s |
| tables / columns / rows / cell text | 8 / 24 / 2000 / 512 bytes |
| status / log / log line | 64 / 500 / 1000 bytes |
| plots / floats | 8 / 65536 |
| images / bytes / side | 4 / 4 MiB / 4096 |
| map points | 2000 |
| events | 32 fields, 100 a second per plugin |
| install package | 64 entries, 128 MiB per file, 256 MiB total |
