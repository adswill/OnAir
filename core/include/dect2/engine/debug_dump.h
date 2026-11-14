// Debug dump: the registers and the stack memory of every thread, written as a minidump (.dmp). No heap (it holds video data and
// would make the file far too big), no symbols (the build server keeps them: cmake target debug_symbols_zip). A developer opens the
// dump with the symbols of the same build in a debugger; tools/analyse_debug_dump.py in the notes does that for a report zip.
//
// Linux x86-64: written by OnAir itself (every thread is asked with a signal for its registers; its stack is then copied as it is).
// Windows: MiniDumpWriteDump from a helper thread. Elsewhere supported() is false and write() says so.
#pragma once
#include <string>

namespace dect2::dump {

// True when this build can write a dump on this system.
bool supported();

// Set up once, early (crash::install() calls it): the signal that asks a thread for its registers, and the buffers, so that a dump
// needs no memory from the heap later (a crash may have broken the heap).
void prepare();

// Write a dump of all threads to path (UTF-8). For normal code: Ctrl+D, the hang watchdog. Threads that do not answer within a
// second are left out (they block signals). False with a reason when it failed. Only one dump at a time: a second call waits for none
// and fails with "busy".
bool write(const std::string& path, std::string& err);

#ifndef _WIN32
// The same from the crash handler: writes to an open file, only async-signal-safe calls. uctx is the crashing thread's ucontext_t,
// sig/si the signal (they go into the dump's exception record). False when the system is not supported or a dump is already running.
bool writeFromSignal(int fd, int sig, const void* si, const void* uctx);
#endif

}
