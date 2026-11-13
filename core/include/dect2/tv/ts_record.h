// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Recording a service to a .ts file (a writer thread behind a bounded queue) and the live buffer (a ring of 188-byte packets) for pause.
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

// Writes chunks to a file on its own thread. write() never blocks on the disk: when the queue is full the chunk is dropped and counted.
class TsWriter {
public:
    ~TsWriter() { close(); }
    bool open(const std::string& path, size_t maxQueueBytes = 32u << 20);
    void close();                                   // flushes what is queued
    bool isOpen() const;
    void write(const uint8_t* p, size_t n);         // whole packets
    uint64_t bytes() const;                         // written to the file
    uint64_t droppedPackets() const;                // dropped because the queue was full
    bool failed() const;                            // a write to the disk failed
    std::string path() const;
    void holdForTest(bool hold);                    // the thread stops draining (tests the overflow)
private:
    void run();
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> q_;
    size_t queued_ = 0, max_ = 0;
    FILE* f_ = nullptr;
    std::thread th_;
    bool stop_ = false, hold_ = false, failed_ = false, open_ = false;
    uint64_t written_ = 0, dropped_ = 0;
    std::string path_;
};

// Ring buffer of transport-stream packets, numbered from 0 for ever. When full the oldest are overwritten. Not thread-safe.
class TsRing {
public:
    void setCapacityPackets(size_t n);              // clears
    void clear() { head_ = 0; }
    size_t capacity() const { return cap_; }
    uint64_t head() const { return head_; }        // index of the next packet to be pushed (= packets pushed so far)
    uint64_t oldest() const { return head_ > cap_ ? head_ - cap_ : 0; }
    void push(const uint8_t* pkt188);
    // Copies packet `index` into out; false when it is older than the buffer holds (or not pushed yet).
    bool read(uint64_t index, uint8_t* out) const;
private:
    std::unique_ptr<uint8_t[]> buf_;
    size_t cap_ = 0;
    uint64_t head_ = 0;
};

// The PCR of a packet in seconds, false when it carries none.
bool tsPcrSeconds(const uint8_t* pkt188, double& sec);

} // namespace dect2
