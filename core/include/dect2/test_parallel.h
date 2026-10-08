// Helper for the tests: run independent cases on a few threads and print their output in a fixed order, so that the log reads the same as a serial run.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {
namespace testpar {

// what one case prints and how many checks it failed (a case never touches shared state)
struct CaseOut {
    std::string text;
    int fails = 0;
    void print(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        text += b;
    }
    void fail(int line, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        char b[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        text += "FAIL line " + std::to_string(line) + ": " + b + "\n";
        fails++;
    }
};

// Runs f(i, out) for i in [0, n) on at most maxThreads threads, starting the cases in `order` (longest first keeps the threads busy; empty = 0..n-1),
// then prints every case's text in index order. Returns the number of failed checks.
template <class F>
int runCases(size_t n, F f, std::vector<size_t> order = {}, unsigned maxThreads = 4) {
    if (order.empty()) for (size_t i = 0; i < n; i++) order.push_back(i);
    std::vector<CaseOut> outs(n);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t k; (k = next.fetch_add(1)) < n;) f(order[k], outs[order[k]]);
    };
    std::vector<std::thread> th;
    for (unsigned i = 1; i < std::min<size_t>(maxThreads, n); i++) th.emplace_back(worker);
    worker();
    for (auto& t : th) t.join();
    int fails = 0;
    for (auto& o : outs) { fputs(o.text.c_str(), stdout); fails += o.fails; }
    fflush(stdout);
    return fails;
}

} // namespace testpar
} // namespace dect2
