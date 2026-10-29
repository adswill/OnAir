// Digital video generator (Tools > Digital video generator): a camera, a still picture, a video file or the transmitter's test picture
// (Test generator), with the overlay, encoded with
// FFmpeg (the codec and settings of videogen_params.h) into one MPEG transport stream, written to a .ts file or sent over UDP / RTP.
// It stands alone: nothing here talks to the receiver or the transmitter (a .ts file it writes can be given to the transmitter as its
// "MPEG-TS / video file").
#pragma once
#include "videogen_overlay.h"
#include "videogen_params.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2::vgen {

// Generator: a copy of the moving test picture of the transmitter's built-in test transport stream (demo_ts.cpp, the one the DVB-T2
// T2Generator carries in its real PLP), drawn at the output resolution (drawTestCard), with a message across it
enum class Source { Camera, Picture, VideoFile, Generator };
enum class AudioSource { Silence, Tone, FromFile, Microphone };
enum class Output { File, Udp, Rtp };
enum class Fit { Stretch, Letterbox, Crop };

struct GenConfig {
    Source source = Source::Camera;
    std::string cameraId;              // empty = the default camera
    std::string path;                  // Picture / VideoFile
    int pattern = 0;                   // Generator: 0 test card, 1 colour bars, 2 grey ramp
    std::string message;               // Generator: the text across the picture (the TX studio's templates: tx_templates.h); empty = none
    AudioSource audio = AudioSource::Tone;
    std::string micId;                 // empty = the default input
    double toneHz = 1000;
    Output output = Output::File;
    std::string tsPath;                // File
    std::string host = "127.0.0.1";    // Udp / Rtp: where to send (unicast or multicast)
    int port = 5000;
    int ttl = 8;
    double seconds = 60;               // File: the length; streaming runs until stopped
    Fit fit = Fit::Letterbox;
    Settings enc;
    Layout overlay;
    std::shared_ptr<const std::vector<uint8_t>> font;   // TrueType for the overlay text
};

struct GenStats {
    bool running = false, finished = false;   // finished: the file is complete (or the stream was stopped)
    double seconds = 0;                       // encoded time
    double progress = 0;                      // File: 0..1
    uint64_t frames = 0, bytes = 0;
    double kbps = 0;                          // measured TS rate
    int lateFrames = 0;                       // live: frames the encoder was too slow for
    std::string videoEncoder, audioEncoder;   // the FFmpeg encoders actually used
    std::string error;
};

// Where the source picture goes in the output picture (even numbers for 4:2:0): the part of the source to use (s*) and where it lands (d*)
struct FitPlan { int sx = 0, sy = 0, sw = 0, sh = 0, dx = 0, dy = 0, dw = 0, dh = 0; };
FitPlan fitPlan(int srcW, int srcH, int dstW, int dstH, Fit f);

// The test generator's picture, frame n of a W x H YUV 4:2:0 picture (even sizes; ys / us / vs are the line strides): picture 0 test
// card (colour bars, a sweep, a chequerboard with a bouncing ball), 1 colour bars, 2 grey ramp. A bar grows along the bottom once a second.
void drawTestCard(uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs, int W, int H, int n, double fps, int picture);

// "rtp://192.168.1.20:5000" / "udp://..."; empty for a file
std::string streamUrl(Output o, const std::string& host, int port);
// What to open in VLC on the receiving PC: rtp://@:5000 (unicast) or rtp://@239.1.1.1:5000 (multicast)
std::string vlcOpenUrl(Output o, const std::string& host, int port);
bool isMulticast(const std::string& host);
// The length of a video file in seconds, 0 when FFmpeg cannot tell
double mediaSeconds(const std::string& path);

class VideoGenerator {
public:
    VideoGenerator();
    ~VideoGenerator();   // stops
    VideoGenerator(const VideoGenerator&) = delete;
    VideoGenerator& operator=(const VideoGenerator&) = delete;

    bool start(const GenConfig& c, std::string& err);   // false with err when a run is going or the config cannot work
    void stop();                                        // stops and waits for the thread (the .ts file is closed properly)
    bool running() const;
    bool wait(double timeoutSeconds);                   // true when the run has ended
    GenStats stats() const;
    void setOverlay(const Layout& l);                   // takes effect on the next frame
    // The latest encoded picture with its overlay, RGBA at a reduced size; false when there is none newer than `serial`
    bool preview(std::vector<uint8_t>& rgba, int& w, int& h, uint64_t& serial) const;

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::vgen
