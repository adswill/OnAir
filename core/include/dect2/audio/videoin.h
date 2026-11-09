// Video capture for the transmitter: cameras and HDMI / SDI capture cards, plus a looping file.
// The device is never opened until start() — TxSession does that only on START TRANSMITTING.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct VideoInputDevice {
    std::string id;     // empty = the default camera
    std::string name;
};

std::vector<VideoInputDevice> listVideoInputs();   // cameras and capture cards; default first when the OS has one

class VideoIn {
public:
    enum Kind { None, Camera, File };
    VideoIn();
    ~VideoIn();
    VideoIn(const VideoIn&) = delete;
    VideoIn& operator=(const VideoIn&) = delete;

    // Camera: deviceId empty = default. File: any container FFmpeg can decode, looped. False with err; nothing left open.
    bool start(Kind k, const std::string& pathOrDevice, std::string& err);
    void stop();
    bool running() const { return running_; }
    // A file plays against the consumer's clock, the time of the picture about to be sent (a file source has no speed of its own: #439); before the
    // first call against the wall clock. waitMs > 0: wait that long at most for the frame of this time (for a consumer that runs faster than real time).
    void setClock(double seconds, int waitMs = 0);
    Kind kind() const { return kind_; }
    int width() const;
    int height() const;

    // Latest frame as analog-TV YUV (Y 0..1, U/V colour difference around 0), bilinear to dw x dh. False if none yet.
    bool copyYuv(int dw, int dh, float* y, float* u, float* v);
    // Latest frame as MPEG-2 YUV 4:2:0 (8-bit planar). Stride 0 = packed (dw, dw/2). False if none yet.
    bool copyYuv420(int dw, int dh, uint8_t* y, uint8_t* u, uint8_t* v, int yStride = 0, int uStride = 0, int vStride = 0);

    struct Impl;
    std::unique_ptr<Impl> p_;

private:
    bool running_ = false;
    Kind kind_ = None;
};

} // namespace dect2
