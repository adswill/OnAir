# Docking the panels (new interface)

**View > Docking > Use docking** (Ctrl+Alt+D, Cmd+Opt+D on macOS) turns the main window into a dock space. Off by default; not with the
touch interface. Turning it off puts everything back the way it was (floating windows, the usual layout).

With docking on:

- The panels (Modes, Spectrum, List, Analysis, Log) and the tool windows (Map, Memory, Passes ...) are windows you can arrange.
- Drag a window by its title: drop targets appear (a cross on every box, one at each edge of the window). Drop on a side to dock it there, in
  the middle to make it a tab of that box. Drag a title or tab out of its box to float it again.
- The bars between boxes resize them.
- Keyboard (also in View > Docking > Window): Ctrl+Alt+arrows dock the focused window at that edge, Ctrl+Alt+Enter makes it a tab of the
  spectrum, Ctrl+Alt+U undocks it, Ctrl+Alt+PageUp / PageDown switch tabs. On macOS Cmd+Opt instead of Ctrl+Alt.
- The layout is kept per mode. **Save layout as / Load layout / Delete layout** keep named layouts (also from other modes);
  **Reset docked layout** and **Undock all** start again. **View > Reset layout** clears the mode's layout too.

Own operating-system windows (experimental): **View > Allow detaching panels (experimental)** (off by default, desktop only), then **View >
Detach panel** > Spectrum, Waterfall or Messages. Each opens as a window of its own that can sit on another monitor; its place and size are
kept. They are read-only copies of what the main window shows (the receiver cannot be tuned from them), drawn like the video pop-out, so the
docked layout, the shortcuts and the touch interface are untouched. Close the window (or untick the entry) to bring it back. The map and the
decoder tabs cannot be detached yet. Dear ImGui's own multi-viewport build is not used: the app's windows are placed in main-window
coordinates. Tabs cannot be reordered by dragging, and the classic interface does not dock.

Developer check: `--dockselftest` runs a hidden test of dragging, the shortcuts and turning docking off (11 checks).
