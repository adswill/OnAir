// Runs independent test cases on up to 4 threads. A case prints through jprintf(); each case has its own output buffer and the buffers are
// written in the order the cases were added, so the log reads the same whatever the threads do. Cases must not share state (CHECK counters
// are atomic, everything else a case touches is its own).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace testjobs {

inline std::string*& sink() { thread_local std::string* s = nullptr; return s; }

// printf into the current case's buffer; plain printf outside a case
inline int jprintf(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return n; }
    std::string s((size_t)n + 1, '\0');
    vsnprintf(&s[0], s.size(), fmt, ap2);
    va_end(ap2);
    s.resize((size_t)n);
    if (std::string* out = sink()) out->append(s);
    else fputs(s.c_str(), stdout);
    return n;
}

class Jobs {
public:
    void add(std::function<void()> f) { fn_.push_back(std::move(f)); }
    void run(unsigned threads = 4) {
        std::vector<std::string> out(fn_.size());
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (size_t i; (i = next++) < fn_.size();) {
                sink() = &out[i];
                fn_[i]();
                sink() = nullptr;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < std::min<size_t>(threads, fn_.size()); t++) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
        for (auto& s : out) fputs(s.c_str(), stdout);
        fflush(stdout);
        fn_.clear();
    }
private:
    std::vector<std::function<void()>> fn_;
};

} // namespace testjobs
