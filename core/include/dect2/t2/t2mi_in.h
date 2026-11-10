// T2-MI ingest (ETSI TS 102 773): finds the T2-MI stream in a transport stream (a PID whose packets carry T2-MI packets with good CRCs, such as a DVB-S feed to a DTT
// transmitter), and rebuilds each PLP's transport stream from its baseband frames. No radio layer is involved: the frames are taken as sent.
#pragma once
#include "dect2/t2/t2fec.h"
#include "dect2/t2/t2mi_out.h"
#include "dect2/tv/bbunpack.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace dect2 {

class T2miIngest {
public:
    using Sink = std::function<void(int plp, const uint8_t* pkt188)>;
    void setSink(Sink s) { sink_ = std::move(s); }
    void feed(const uint8_t* pkt188);          // every TS packet of the outer stream
    bool found() const { return pid_ >= 0; }
    int pid() const { return pid_; }
    std::vector<int> plps() const;
    uint64_t packets(int plp) const;
    uint64_t framesLost(int plp) const;
    void reset();
private:
    void drain(t2mi::Extract& e);
    Sink sink_;
    int pid_ = -1;
    std::map<int, std::unique_ptr<t2mi::Extract>> probe_;
    std::map<int, BbUnpacker> plp_;
    std::map<int, uint64_t> count_;
    std::map<int, int> lastIdx_;
};

}  // namespace dect2
