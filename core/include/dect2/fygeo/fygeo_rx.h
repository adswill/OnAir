// FengYun geostationary receiver: FY-4A / FY-4B LRIT and FY-4A HRIT-II/III (DVB-S2, QPSK, CCSDS xRIT files with JPEG 2000 pictures) and FY-2 S-VISSR
// (uncoded differential BPSK, 660 ksym/s, scan lines of IR1 - IR4 and VIS). Receive only; needs a dish (about 1 m or more) with an LNA at 1.7 GHz.
//
// FY-4: the DVB-S2 receiver of the DVB-S/S2 mode (dvbs_rx.h) does the demodulation and error correction with the symbol rate of the link, the
// transport stream packets are searched for CADUs on any PID, and the VCDUs go through the xRIT layers of goes_proto.h (packet zone 1012 bytes) to
// whole files; the files are read in FY-4's header dialect and the JPEG 2000 segments are put together into pictures per channel and region.
// FY-2: a matched filter, Gardner timing and Costas loop, differential decoding, the PN preamble and frame sync, descrambling and the scan lines.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct FygeoTelemetry;   // fygeo_tel.h

class FygeoReceiver {
public:
    FygeoReceiver();
    ~FygeoReceiver();

    void configure(double inputRateHz);
    void setLink(int link);                 // 0 FY-4A LRIT, 1 FY-4B LRIT, 2 FY-4A HRIT-II/III, 3 FY-2 S-VISSR
    int link() const;
    void setLritSatellite(int sat);         // the satellite of the LRIT mode: 0 FY-4A (90 ksym/s), 1 FY-4B (120 ksym/s); a running LRIT receiver switches at once
    int lritSatellite() const;
    void setSymbolRate(double hz);          // 0 = the link's own (90 / 120 / 1000 / 660 ksym/s); anything else is used as given
    double symbolRate() const;
    void setSignalOffset(double hz);        // S-VISSR: where the carrier is expected; the FY-4 DVB-S2 receiver finds its own carrier
    bool ready() const;
    void reset();                           // the demodulator and partial files; the products stay
    void clearProducts();
    void feed(const cf32* x, size_t n);
    bool telemetry(FygeoTelemetry& out, uint64_t lastSeq);
    // the products of each link are kept apart (the four modes share this receiver); link -1 = the current one
    void clearResults(int link = -1);
    std::string saveResults(int link = -1) const;
    bool loadResults(const std::string& blob, int link = -1);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // every whole xRIT file (FY-4, transport header removed: its headers and data) from the receiver's thread
    void setFileCallback(std::function<void(const std::vector<uint8_t>&, int vcid, int apid)> cb);
    // every CADU found (FY-4: 1024 bytes) and every S-VISSR frame (44356 bytes, descrambled) as they come
    void setCaduCallback(std::function<void(const uint8_t*)> cb);
    void setSvissrFrameCallback(std::function<void(const uint8_t*)> cb);
    // FY-4: the CADUs of a .cadu file (1024 byte frames) through the file layers; S-VISSR: the frames of a .svissr file (44356 bytes, descrambled)
    // done on the analysis thread at its next feed(), so the mode must be running
    void importFrames(std::vector<uint8_t> data);
    void flush();                           // FY-4: waits until everything fed so far is decoded and delivered (recordings, tests; not the engine)
    void setBlocking(bool on);              // FY-4: recordings: wait for the decoder instead of dropping frames when behind

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning fy4LritTuning();
ModeTuning fy4aHritTuning();
ModeTuning fy2SvissrTuning();

} // namespace dect2
