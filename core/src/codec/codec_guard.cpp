// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The guard around user-installed voice codecs (see codec_guard.h).
#include "dect2/codec/codec_guard.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csetjmp>
#include <csignal>
#endif

namespace dect2::codec {

namespace {

// ------------------------------------------------------------------ a crash inside the call, caught on the codec thread

#ifdef _WIN32
// A vectored handler puts the thread back where RtlCaptureContext was called (the same as a longjmp), with tFaulted set.
thread_local CONTEXT* tCtx = nullptr;
thread_local volatile LONG tFaulted = 0;

LONG CALLBACK onFault(EXCEPTION_POINTERS* ep) {
    if (!tCtx) return EXCEPTION_CONTINUE_SEARCH;
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = tCtx;
    tCtx = nullptr;
    tFaulted = 1;
    *ep->ContextRecord = *c;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__attribute__((noinline)) bool faultSafeCall(const std::function<void()>& f) {
    static std::once_flag once;
    std::call_once(once, [] { AddVectoredExceptionHandler(1, onFault); });
    alignas(16) CONTEXT ctx;
    tFaulted = 0;
    RtlCaptureContext(&ctx);
    if (tFaulted) { tFaulted = 0; return false; }   // back here from onFault
    tCtx = &ctx;
    f();
    tCtx = nullptr;
    return true;
}
#else
thread_local sigjmp_buf* tJmp = nullptr;
struct sigaction gPrev[NSIG];
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};

void onFault(int sig, siginfo_t*, void*) {
    if (sigjmp_buf* j = tJmp) { tJmp = nullptr; siglongjmp(*j, sig); }
    // not inside a codec call: the handler that was there before (the crash report) takes it
    sigaction(sig, &gPrev[sig], nullptr);
    raise(sig);
}

bool faultSafeCall(const std::function<void()>& f) {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa {};
        sa.sa_sigaction = onFault;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        for (int s : kSignals) sigaction(s, &sa, &gPrev[s]);
    });
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1) != 0) { tJmp = nullptr; return false; }   // back here from onFault
    tJmp = &jb;
    f();
    tJmp = nullptr;
    return true;
}
#endif

// ------------------------------------------------------------------ the codec worker thread

struct Job {
    std::function<void()> work;
    std::mutex m;
    std::condition_variable cv;
    int state = 0;   // 0 waiting or running, 1 returned, 2 crashed
};

struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Job>> q;
    bool dead = false;   // left behind with a call that does not return: it ends when (if) that call comes back
};

std::mutex gMu;
std::shared_ptr<Worker> gWorker;
std::map<std::string, std::string> gDisabled;   // id -> message
std::deque<std::string> gNotices;

void loop(std::shared_ptr<Worker> w) {
    for (;;) {
        std::shared_ptr<Job> j;
        {
            std::unique_lock<std::mutex> lk(w->m);
            w->cv.wait(lk, [&] { return w->dead || !w->q.empty(); });
            if (w->q.empty()) return;
            j = w->q.front();
            w->q.pop_front();
        }
        const bool ok = faultSafeCall(j->work);
        // after a crash the closure may hold damaged objects: it is kept, never destroyed
        if (!ok) new std::function<void()>(std::move(j->work));
        { std::lock_guard<std::mutex> lk(j->m); j->state = ok ? 1 : 2; }
        j->cv.notify_all();
    }
}

std::shared_ptr<Worker> startWorker() {
    auto w = std::make_shared<Worker>();
    std::thread(loop, w).detach();
    return w;
}

// a call on w does not return: a new worker takes the queue (the calls of the other codecs) and w is left to its call
void abandon(const std::shared_ptr<Worker>& w) {
    std::lock_guard<std::mutex> lk(gMu);
    if (gWorker != w) return;
    gWorker = startWorker();
    std::deque<std::shared_ptr<Job>> rest;
    { std::lock_guard<std::mutex> l2(w->m); w->dead = true; rest.swap(w->q); }
    w->cv.notify_all();
    { std::lock_guard<std::mutex> l3(gWorker->m); for (auto& j : rest) gWorker->q.push_back(j); }
    gWorker->cv.notify_one();
}

std::string label(const std::string& id) {
    if (id == "mbelib") return "library (mbelib)";
    return "plug-in (" + id + ")";
}

void disable(const std::string& id, bool crashed) {
    const std::string msg = "The voice codec " + label(id) + (crashed ? " crashed" : " stopped answering") +
                            " and was disabled: it is probably built for a different library version (libmbe-neo?). "
                            "Calls and data are still shown, without sound. See Help > Voice codecs.";
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (gDisabled.count(id)) return;
        gDisabled[id] = msg;
        gNotices.push_back(msg);
    }
    fprintf(stderr, "%s\n", msg.c_str());
    fflush(stderr);
}

}  // namespace

GuardResult runGuarded(const std::string& id, std::function<void()> work, int timeoutMs) {
    if (guardDisabled(id)) return GuardResult::Disabled;
    auto j = std::make_shared<Job>();
    j->work = std::move(work);
    std::shared_ptr<Worker> w;
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gWorker) gWorker = startWorker();
        w = gWorker;
        std::lock_guard<std::mutex> l2(w->m);
        w->q.push_back(j);
    }
    w->cv.notify_one();
    std::unique_lock<std::mutex> lk(j->m);
    if (!j->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return j->state != 0; })) {
        lk.unlock();
        disable(id, false);
        abandon(w);
        return GuardResult::TimedOut;
    }
    if (j->state == 2) {
        lk.unlock();
        disable(id, true);
        return GuardResult::Crashed;
    }
    return GuardResult::Ok;
}

bool guardDisabled(const std::string& id) {
    std::lock_guard<std::mutex> lk(gMu);
    return gDisabled.count(id) > 0;
}

std::string guardMessage(const std::string& id) {
    std::lock_guard<std::mutex> lk(gMu);
    const auto it = gDisabled.find(id);
    return it == gDisabled.end() ? std::string() : it->second;
}

void guardReset(const std::string& id) {
    std::lock_guard<std::mutex> lk(gMu);
    gDisabled.erase(id);
}

std::string guardTakeNotice() {
    std::lock_guard<std::mutex> lk(gMu);
    if (gNotices.empty()) return {};
    std::string s = gNotices.front();
    gNotices.pop_front();
    return s;
}

std::string guardAllMessages() {
    std::lock_guard<std::mutex> lk(gMu);
    std::string s;
    for (const auto& kv : gDisabled) s += (s.empty() ? "" : "\n") + kv.second;
    return s;
}

}  // namespace dect2::codec
