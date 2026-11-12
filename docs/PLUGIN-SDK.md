# Plug-in SDK: what exists, what was missing

Status on integ-36 (2026-11-03). Full reference: `docs/plugins.md`, `docs/automations.md` is `core/src/automation/README.md`, remote API: `docs/REMOTE-API.md`, `docs/MCP.md`.

## What exists

- **Stable C ABI**: `sdk/include/onair_plugin.h` (CC0, `abi_version` 1, struct sizes first, host function table; IQ in, tables/status/log/plots/images/map out, events, optional audio out, declared settings).
- **SDK folder**: `sdk/template`, three examples (`tone-detector`, `burst-counter`, `log-to-file`), `sdk/cmake/OnAirPlugin.cmake` for a standalone build, a dependency-free Rust crate.
- **Host** (`core/src/plugin/`): manifest parser with bounds, ABI-mismatch refusal with a reason, loader, in-process runner with a 2 s watchdog, helper-process runner (`onair-plugin-host`), crash marker and safe mode, permissions, `.onairplugin` install, stable mode numbers.
- **UI**: View > Plugins manager (loaded/failed reason, log, permissions, safe mode); plug-in modes in the mode list under PLUGINS; the safety note is in the manager and docs.
- **Scripting hook**: Automations (`View > Automations`): rules with an `run` action (program + args templated with event fields), Lua scripts, webhook. Running programs is off until "Allow running programs" is ticked; a rule that runs a program needs confirmation.
- **Tests**: `tests/plugin/test_plugin.cpp` (fake plug-in load, wrong ABI refused, crash/hang contained, failed plug-in switched off, safe mode), `tests/automation`.

## What was missing (and now done)

- This document.
- A **Settings** page ("Plug-ins and scripts"): one place with the native-code safety note, a button to the plug-in manager, a button to Automations and the current state of "running programs" (off by default). Strings translated in de and lt.

## Plugin store (3.9)

A signed static registry with a Store tab, a validator and signing tool: `docs/PLUGIN-MARKETPLACE-3.9.md`.

## Community marketplace (4.0)

Upload from the app (GitHub sign-in), likes, download counts, search; uploads are live at once and marked unreviewed: `docs/PLUGIN-MARKETPLACE-4.0.md`.

## Not done

- Sandboxed helper process (macOS sandbox profile, Linux seccomp) for untrusted plug-ins.
- Windows helper-process support.
- Audio out is plumbed (`audio_out`, needs `audio-out` permission); no bundled example uses it.
