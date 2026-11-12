// Debug report: Ctrl+D (Cmd+D), Help > Debug report, a crash (at the next start) or a hang (the watchdog) makes a debug dump: the stacks
// of all threads (core debug_dump.h; no heap, no symbols), this computer (sysinfo.h), at most 1 MB of the newest log and a screenshot
// of the OnAir window. A window then asks what the user was doing, how to reproduce it and what the problem is; Save writes one zip
// (report.txt, dump.json, dump.dmp, system.txt, log.txt, crash.txt, screenshot.png) to attach to a GitHub issue. Nothing is uploaded.
// The developer opens the dump with the symbols of the same build (cmake target debug_symbols_zip; the analysis script in the notes).
#pragma once
#include <string>

struct App;

// At start, after startApp(): picks up the crash or hang dump of the last run (the window opens once the crash note is closed) and
// starts the hang watchdog. devRun (a start with arguments: --shot, tests): no watchdog and no window for the last run.
void debugReportInit(App& a, bool devRun);
// Every frame, between NewFrame and Render: the heartbeat for the watchdog, Ctrl+D, the log file, and the window.
void debugReportFrame(App& a);
// Every pass of the main loop, also while the window is minimised (no frame is drawn then): the main thread is alive.
void debugReportBeat();
// Help > Debug report, the command palette: the same as Ctrl+D (the dump and the screenshot are taken in this frame).
void debugReportRequest();
// At exit: stops the watchdog.
void debugReportShutdown();
