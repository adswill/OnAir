// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native driver for the RFNM (rfnm.com): a motherboard with up to two daughterboards (Granita, Lime, Breakout, XMC_RFFE, ...), each with one
// or more receive channels, on USB or Ethernet. Unlike the other native radios its library, librfnm, has a C++ interface only: it cannot be
// loaded at run time (native_common.h), so this file is built and linked only when CMake finds librfnm (DECT2_WITH_RFNM -> DECT2_HAVE_RFNM).
//
// One entry per receive channel. librfnm numbers the channels in the order the boards register: slot 0's, then slot 1's. The listing only
// has the boards' EEPROM data (device::find() carries no channel data), so a channel's gain and frequency range come from the radio once it
// has been opened; until then they are guessed from the board.
// The samples arrive as float I/Q (STREAM_FORMAT_CF32), full scale 1.0, which is cf32 as it is.
// Not supported yet: channels that carry a real signal (the XMC_RFFE's HF input, on the I pin), and the Breakout's second channel, whose I and
// Q pins are swapped.
#include "native_common.h"
#include <librfnm/device.h>
#include <librfnm/rx_stream.h>
#include <map>

namespace dect2 {
namespace native {

namespace {

constexpr double kTopRateHz = 122.88e6;         // the rates are 122.88 MHz / 2^n ...
constexpr int kRateSteps = 5;                   // ... down to 7.68 Msps
constexpr size_t kBlockGranularity = 32768;     // rx_stream::read() takes multiples of this
constexpr int kStallS = 5;
constexpr uint8_t kBoardBreakout = 1, kBoardXmcRffe = 6;   // EEPROM board ids (the RFNM kernel tree's rfnm-shared.h, not librfnm's headers)

// ascending
std::vector<double> rfnmRates() {
    std::vector<double> v;
    for (int n = kRateSteps - 1; n >= 0; n--) v.push_back(kTopRateHz / (1 << n));
    return v;
}

std::string mhzText(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.6g MHz", hz / 1e6);
    return b;
}

std::string failText(rfnm_api_failcode c) {
    const char* s = rfnm::device::failcode_to_string(c);
    return s && *s ? std::string(s) : "error " + std::to_string((int)c);
}

std::string serialOf(const rfnm_dev_hwinfo& hw) {
    return trimmed(reinterpret_cast<const char*>(hw.motherboard.serial_number), sizeof hw.motherboard.serial_number);
}

std::string transportText(rfnm::transport t) { return t == rfnm::TRANSPORT_TCP ? "tcp" : t == rfnm::TRANSPORT_LOCAL ? "local" : "usb"; }

// What a channel offers. The radio says it once it is open; before that, a guess from the board
struct ChannelRange {
    double gainMin = 0, gainMax = 60;
    double freqMin = 0, freqMax = 0;   // 0: not known
    bool real = false;                 // a real signal on one pin (not supported yet)
};

ChannelRange guessedRange(uint8_t boardId, int onBoard) {
    ChannelRange r;
    if (boardId == kBoardXmcRffe) {
        if (onBoard == 1) r.real = true;   // HF, 0 to 60 MHz, sampled directly
        else { r.gainMin = -63; r.gainMax = 26; r.freqMin = 60e6; r.freqMax = 6e9; }
    } else if (boardId == kBoardBreakout) {
        r.gainMin = r.gainMax = 0;   // no gain stage
    }
    return r;
}

// find() and opening take turns: both go through the USB stack, and a radio this program has open is not listed again (gKnown stands in)
std::mutex gMu;
int gOpen = 0;
std::vector<DeviceInfo> gKnown;
std::map<std::string, ChannelRange> gRanges;   // per "serial#ch=n", what the radio said when it was last opened

std::string rangeKey(const std::string& serial, uint32_t ch) { return serial + "#ch=" + std::to_string(ch); }

std::vector<RadioSetting> rfnmSettings() {
    return {ppmSetting(0.01),
            choiceSetting("path", "RF input",
                          "The input of the daughterboard the channel listens on. Automatic: the one the board prefers for this channel.\n"
                          "A board refuses an input it does not have on this channel: the automatic one is used then.",
                          {"auto", "a", "b", "c", "d", "e", "f", "g", "h", "embed"},
                          {"Automatic", "SMA A", "SMA B", "SMA C", "SMA D", "SMA E", "SMA F", "SMA G", "SMA H", "Embedded antenna"}, "auto")};
}

} // namespace

class RfnmSource : public NativeSource {
public:
    // nativeArgs: "<usb|tcp|local>:<address>#ch=<n>" (listRfnm)
    explicit RfnmSource(const DeviceInfo& d) : serial_(d.serial) {
        std::string a = d.nativeArgs;
        const size_t cut = a.rfind("#ch=");
        if (cut != std::string::npos) { ch_ = (uint32_t)strtoul(a.c_str() + cut + 4, nullptr, 10); a.erase(cut); }
        const size_t colon = a.find(':');
        const std::string t = a.substr(0, colon);
        transport_ = t == "tcp" ? rfnm::TRANSPORT_TCP : t == "local" ? rfnm::TRANSPORT_LOCAL : rfnm::TRANSPORT_USB;
        address_ = colon == std::string::npos ? std::string() : a.substr(colon + 1);
    }
    ~RfnmSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (dev_) return apply(s, err, false);   // (not reached: closeDevice() comes first)
        try {
            dev_ = new rfnm::device(transport_, address_);   // throws when it cannot
        } catch (const std::exception& e) {
            dev_ = nullptr;
            err = std::string("RFNM: cannot open the radio (") + e.what() + "): it is in use by another program, unplugged or not reachable";
            if (kOnWindows && transport_ == rfnm::TRANSPORT_USB) err += std::string(". If it is plugged in: ") + windowsUsbDriverHint();
            return false;
        }
        gOpen++;
        const rfnm_api_rx_ch* c = ch_ < dev_->get_rx_channel_count() ? dev_->get_rx_channel(ch_) : nullptr;
        if (!c) { err = "RFNM: the radio has no receive channel " + std::to_string(ch_) + " (another daughterboard?)"; openFailure_ = OpenFailure::Final; return false; }
        info_ = *c;
        const rfnm_dev_hwinfo* hw = dev_->get_hwinfo();
        const std::string sn = hw ? serialOf(*hw) : serial_;
        {   // for the listing (gMu is held)
            ChannelRange r;
            r.gainMin = info_.gain_range.min; r.gainMax = info_.gain_range.max;
            if (r.gainMax <= r.gainMin) r.gainMin = r.gainMax = 0;
            r.freqMin = (double)info_.freq_min; r.freqMax = (double)info_.freq_max;
            r.real = info_.data_type == RFNM_CH_DATA_TYPE_REAL;
            gRanges[rangeKey(sn, ch_)] = r;
        }
        fprintf(stderr, "RFNM: opened %s, channel %u (slot %d, board channel %d): %s to %s, gain %d to %d dB, %s\n", sn.c_str(), ch_, info_.dgb_id, info_.dgb_ch_id,
                mhzText((double)info_.freq_min).c_str(), mhzText((double)info_.freq_max).c_str(), (int)info_.gain_range.min, (int)info_.gain_range.max,
                info_.data_type == RFNM_CH_DATA_TYPE_REAL ? "real" : "complex");
        fflush(stderr);
        if (info_.data_type == RFNM_CH_DATA_TYPE_REAL) {
            err = "RFNM: channel " + std::to_string(ch_) + " carries a real signal (a directly sampled input), which OnAir does not support yet";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (const rfnm_api_failcode r = dev_->set_stream_format(rfnm::STREAM_FORMAT_CF32); r != RFNM_API_OK) {
            err = "RFNM: the radio refused float samples (" + failText(r) + ")";
            return false;
        }
        return apply(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        return apply(s, err, live);
    }
    // safe twice, after a failed open and after an unplug (the stream thread has deleted its stream by then)
    void closeDevice() override {
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return;
        delete dev_;
        dev_ = nullptr;
        gOpen--;
    }
    void streamLoop() override {
        rfnm::rx_stream* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(api_);
            if (!dev_ || !run_) return;
            try {
                st = dev_->rx_stream_create(rfnm::channel_flags[ch_]);
            } catch (const std::exception& e) {
                fprintf(stderr, "RFNM: cannot create the sample stream (%s)\n", e.what());
                fflush(stderr);
            }
            if (!st) return;
            st->set_auto_dc_offset(true, rfnm::channel_flags[ch_]);
            std::lock_guard<std::mutex> sl(streamMu_);
            stream_ = st;
        }
        // about 10 ms per read, in the multiples read() takes
        const size_t block = std::max(kBlockGranularity, (size_t)(rate_ * 0.01) / kBlockGranularity * kBlockGranularity);
        conv_.resize(block);
        try {
            if (const rfnm_api_failcode r = st->start(); r != RFNM_API_OK) {
                fprintf(stderr, "RFNM: the sample stream did not start (%s)\n", failText(r).c_str());
            } else {
                auto last = std::chrono::steady_clock::now();
                while (run_) {
                    void* buffs[] = {conv_.data()};
                    size_t n = 0;
                    uint64_t ns = 0;
                    const rfnm_api_failcode r = st->read(buffs, block, n, ns, 100000);
                    if (!run_) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (n > 0) { push(conv_.data(), n); last = now; }
                    if (r == RFNM_API_USB_FAIL) { fprintf(stderr, "RFNM: USB failure, stopping the source\n"); break; }
                    // a stream that brings nothing for kStallS seconds is treated as lost, so that the radio is opened again
                    if (n == 0 && now - last > std::chrono::seconds(kStallS)) { fprintf(stderr, "RFNM: no samples for %d s (%s), stopping the source\n", kStallS, failText(r).c_str()); break; }
                }
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "RFNM: the sample stream failed (%s)\n", e.what());
        }
        fflush(stderr);
        {
            std::lock_guard<std::mutex> sl(streamMu_);
            st->stop();
            stream_ = nullptr;
        }
        delete st;
    }
    // stopping the stream makes a blocking read() return
    void interrupt() override {
        std::lock_guard<std::mutex> sl(streamMu_);
        if (stream_) stream_->stop();
    }

private:
    // (under api_) live: the stream is running; the sample rate changes only with the stream stopped (NativeSource::retune), since a new
    // rate resets the radio's stream
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        if (!dev_) return false;
        if (!live) {
            // the smallest rate that is fast enough
            const std::vector<double> rates = rfnmRates();
            const double want = s.sampleRate > 0 ? s.sampleRate : rates.front();
            double r = rates.back();
            for (double x : rates) if (x >= want * 0.999) { r = x; break; }
            if (const rfnm_api_failcode e = dev_->set_samp_rate((uint64_t)std::llround(r)); e != RFNM_API_OK) {
                err = "RFNM: the sample rate of " + mhzText(r) + " was refused (" + failText(e) + ")";
                return false;
            }
            rate_ = r;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const double f = std::max(s.centerHz, 0.0);
        const bool ranged = info_.freq_max > info_.freq_min;
        if (ranged) addNote(err, freqSpecNote("RFNM channel", f, (double)info_.freq_min, (double)info_.freq_max));
        // librfnm has no frequency correction: a correction the user set is taken out of the frequency. It refuses a frequency outside the
        // channel's range, so that is clamped.
        int64_t tune = std::llround(ppmCorrectedHz(f, radioPpm(s)));
        if (ranged) tune = std::clamp<int64_t>(tune, info_.freq_min, info_.freq_max);
        dev_->set_rx_channel_freq(ch_, tune, false);
        if (info_.gain_range.max > info_.gain_range.min) {
            const long g = std::clamp(std::lround(s.gainDb), (long)info_.gain_range.min, (long)info_.gain_range.max);
            dev_->set_rx_channel_gain(ch_, (int8_t)g, false);
        }
        // the RFIC's low-pass in MHz (boards without an RFIC, the Breakout and the XMC_RFFE, ignore it)
        const int16_t bw = s.basebandFilterHz > 0 ? (int16_t)std::clamp(std::lround(s.basebandFilterHz / 1e6), 1L, 100L)
                                                  : rfnm::device::suggested_lpf_bw((uint64_t)rate_);
        dev_->set_rx_channel_rfic_lpf_bw(ch_, bw, false);
        dev_->set_rx_channel_path(ch_, pathFor(s, err), false);
        // A retune does not wait for the radio to confirm (up to a second); opening does, and reports what it refused
        const rfnm_api_failcode r = dev_->apply(rfnm::rx_channel_apply_flags[ch_], !live);
        if (r != RFNM_API_OK) {
            const std::string why = "RFNM: the radio refused the settings (" + failText(r) + "): " + dev_->get_apply_error((uint8_t)ch_, false);
            if (live) { fprintf(stderr, "%s\n", why.c_str()); fflush(stderr); return true; }
            err = why;
            if (r == RFNM_API_TUNE_FAIL || r == RFNM_API_GAIN_FAIL) openFailure_ = OpenFailure::Final;
            return false;
        }
        return true;
    }
    rfnm_rf_path pathFor(const TuneSettings& s, std::string& err) const {
        const std::string want = radioOption(s, "path", "auto");
        if (want == "auto") return info_.path_preferred;
        rfnm_rf_path asked = RFNM_PATH_NULL;
        if (want == "embed") asked = RFNM_PATH_EMBED_ANT;
        else if (want.size() == 1 && want[0] >= 'a' && want[0] <= 'h') asked = static_cast<rfnm_rf_path>(RFNM_PATH_SMA_A + (want[0] - 'a'));
        for (int i = 0; i < 10 && info_.path_possible[i] != RFNM_PATH_NULL; i++)
            if (info_.path_possible[i] == asked) return asked;
        addNote(err, "this RFNM channel has no input " + want + ", using " + rfnm::device::rf_path_to_string(info_.path_preferred));
        return info_.path_preferred;
    }

    std::string serial_;
    rfnm::transport transport_ = rfnm::TRANSPORT_USB;
    std::string address_;              // the USB serial, or the IPv4 address
    uint32_t ch_ = 0;                  // librfnm's receive channel number
    rfnm::device* dev_ = nullptr;
    rfnm_api_rx_ch info_{};            // the channel as the radio described it when it was opened
    std::mutex api_;                   // the control calls (open, close, retune) and the stream's creation
    std::mutex streamMu_;              // stream_ against interrupt() from stop()
    rfnm::rx_stream* stream_ = nullptr;
};

void listRfnm(std::vector<DeviceInfo>& out) {
    std::lock_guard<std::mutex> g(gMu);
    if (gOpen > 0 && !gKnown.empty()) { for (const auto& d : gKnown) out.push_back(d); return; }
    std::vector<rfnm::dev_info> found;
    try {
        found = rfnm::device::find(networkDiscovery() ? rfnm::TRANSPORT_FIND : rfnm::TRANSPORT_USB);   // no network search unless allowed (#215)
    } catch (const std::exception& e) {
        static std::string logged;
        if (logged != e.what()) { logged = e.what(); fprintf(stderr, "RFNM: the radio search failed (%s)\n", e.what()); fflush(stderr); }
        return;
    }
    // in the log file as well, when it changes: when the radio does not show up this says whether the library saw it
    static size_t logged = (size_t)-1;
    if (found.size() != logged) {
        logged = found.size();
        fprintf(stderr, "RFNM: librfnm %s lists %zu radio(s)\n", RFNM_VERSION, found.size());
        fflush(stderr);
    }
    std::vector<DeviceInfo> list;
    const std::vector<double> rates = rfnmRates();
    // a radio on USB that is also on the network is found twice: the USB one is listed
    auto onUsb = [&](const std::string& sn) {
        for (const auto& o : found) if (o.transport == rfnm::TRANSPORT_USB && serialOf(o.hwinfo) == sn) return true;
        return false;
    };
    for (const auto& f : found) {
        const rfnm_dev_hwinfo& hw = f.hwinfo;
        const std::string sn = serialOf(hw);
        if (f.transport != rfnm::TRANSPORT_USB && !sn.empty() && onUsb(sn)) continue;
        const bool both = hw.daughterboard[0].board_id && hw.daughterboard[1].board_id;
        uint32_t ch = 0;   // librfnm's numbering: slot 0's channels, then slot 1's
        for (int slot = 0; slot < 2; slot++) {
            const rfnm_dev_hwinfo_bit& db = hw.daughterboard[slot];
            if (!db.board_id) continue;
            const std::string board = trimmed(db.user_readable_name, sizeof db.user_readable_name);
            for (int c = 0; c < db.rx_ch_cnt; c++, ch++) {
                ChannelRange r = guessedRange(db.board_id, c);
                const auto it = gRanges.find(rangeKey(sn, ch));
                if (it != gRanges.end()) r = it->second;
                if (r.real) continue;   // not supported yet
                DeviceInfo d;
                d.kind = DeviceInfo::Native;
                d.board = "rfnm";
                d.serial = sn;
                d.nativeArgs = transportText(f.transport) + ":" + f.address + "#ch=" + std::to_string(ch);
                d.name = "RFNM " + (both ? "slot " + std::to_string(slot) + " " : std::string()) + (board.empty() ? "board" : board) + " CH" + std::to_string(c) +
                         (sn.empty() ? "" : " " + sn) + (f.transport == rfnm::TRANSPORT_TCP ? " (network, experimental)" : " (experimental)");
                d.minRateHz = rates.front(); d.maxRateHz = rates.back();
                for (double x : rates) d.rateRanges.emplace_back(x, x);
                d.gainMinDb = r.gainMin; d.gainMaxDb = r.gainMax;
                d.minFreqHz = r.freqMin; d.maxFreqHz = r.freqMax;
                d.settings = rfnmSettings();
                list.push_back(d);
            }
        }
    }
    gKnown = list;
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makeRfnm(const DeviceInfo& d) {
    return std::make_unique<RfnmSource>(d);
}

} // namespace native
} // namespace dect2
