// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A user-installed voice codec (a plug-in, or mbelib / mbelib-neo loaded directly) is code OnAir did not build: one built for another
// library version can crash, scribble over memory or never return. Every call into such a codec goes through runGuarded():
//  - it runs on a codec worker thread, never on the caller's (the receiver keeps its spectrum and its timing even when a codec hangs);
//  - a crash inside it (SIGSEGV, SIGBUS, SIGILL, SIGFPE; on Windows an access violation, illegal instruction ...) is caught on that thread;
//  - a call that does not return within the time limit is given up (its thread is left behind, a new worker takes the other codecs).
// After the first crash or hang the codec is disabled for the rest of the run (Look again in the codec help clears it) and a plain-words
// message says so. The work must own everything it touches (it may outlive runGuarded() when it hangs): copy the inputs into a
// heap object held by the closure, copy the outputs back only on Ok.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace dect2::codec {

enum class GuardResult { Ok, Crashed, TimedOut, Disabled };

// Runs work for the codec `id` ("ambe2", "mbelib" ...) with the guard. Ok: it returned normally.
GuardResult runGuarded(const std::string& id, std::function<void()> work, int timeoutMs = 1500);

bool guardDisabled(const std::string& id);
// The plain-words message for a disabled codec (empty when it works).
std::string guardMessage(const std::string& id);
// Clears the disabled state (Look again / tests).
void guardReset(const std::string& id);
// A message not shown yet (one per fault), for the app's one-time popup; empty when none.
std::string guardTakeNotice();
// Every disabled codec's message, one per line.
std::string guardAllMessages();

}  // namespace dect2::codec
