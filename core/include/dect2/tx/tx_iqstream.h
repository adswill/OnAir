// The IQ of a transmission on a TCP socket: GNU Radio (or any program) connects and receives the samples OnAir is generating, whether they go to a radio,
// an IQ file or the "Test (dry run, no RF)" sink. Plain TCP, no extra library. It never blocks or refuses a transmission: a client that cannot keep up
// loses blocks (whole blocks, so the sample stream stays aligned), a client that goes away is dropped, and a port that cannot be opened is a warning.
//
// Data port (default 7355): after the connect, raw samples in the chosen format (cf32 / cs16 / ci16 / cs8 / cu8, little endian, I then Q).
//   cs16 and ci16 are the same bytes (the second is the SigMF name). cs8 is signed, cu8 is unsigned with 127.5 as zero (RTL-SDR style).
//   headerLine = true: the stream starts with one JSON line (ending in \n) before the samples.
// Side channel (data port + 1, when metaPort is on): connect, read one JSON line, the connection is closed. It is always available, also while no
//   transmission runs (then "active":false), so a flow graph can learn the rate and the centre frequency without touching the data stream.
// The JSON line: {"onair_tx_iq":1,"format":"cf32","sample_rate":2000000,"center_freq":522000000,"mode":"dvbt2","active":true}
#pragma once
#include <complex>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {
using cf32 = std::complex<float>;

enum class TxIqFormat { Cf32 = 0, Cs16 = 1, Ci16 = 2, Cs8 = 3, Cu8 = 4 };
const char* txIqFormatName(TxIqFormat f);
bool txIqFormatParse(const std::string& s, TxIqFormat& f);   // case-insensitive
size_t txIqBytesPerSample(TxIqFormat f);
// n complex samples to bytes of the format (out gets n * txIqBytesPerSample bytes); values are clipped to full scale
void txIqPack(const cf32* x, size_t n, TxIqFormat f, uint8_t* out);

struct TxIqStreamConfig {
    bool enabled = false;
    int port = 7355;
    std::string bindAddr = "127.0.0.1";   // local only by default; "0.0.0.0" for the network
    TxIqFormat format = TxIqFormat::Cf32;
    bool headerLine = false;              // one JSON line before the samples on the data port
    bool metaPort = true;                 // the side channel on port + 1
    size_t maxPendingBytes = 32u << 20;   // per client; beyond it blocks are dropped for that client
};

class TxIqStream {
public:
    TxIqStream();
    ~TxIqStream();
    TxIqStream(const TxIqStream&) = delete;
    TxIqStream& operator=(const TxIqStream&) = delete;
    // Opens the sockets. False with a plain-words reason (port in use, bad address); the caller shows it as a warning and goes on transmitting.
    bool start(const TxIqStreamConfig& c, double sampleRateHz, double centerHz, const std::string& modeId, std::string& err);
    void stop();
    // Hands a block to every client; never blocks. Safe to call from the sender thread.
    void push(const cf32* x, size_t n);
    int clients() const;
    int port() const;   // the data port actually bound (useful when 0 was asked for)
    uint64_t samplesSent() const;
    uint64_t blocksDropped() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The setting the transmit session reads when it starts (the app sets it from its preferences; a test sets it directly)
void txIqStreamSetConfig(const TxIqStreamConfig& c);
TxIqStreamConfig txIqStreamConfig();

}  // namespace dect2
