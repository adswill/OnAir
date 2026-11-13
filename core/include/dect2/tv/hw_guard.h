// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hardware video decoding start guard. A stream joined mid-GOP gives the GPU decoder (VideoToolbox, Direct3D 11) pictures it must skip
// (RASL / trailing pictures without their IRAP); it reports no error and simply stays silent. The guard (1) holds packets back
// until a keyframe so the GPU session starts on an IRAP, and (2) says "use the CPU decoder" when the GPU has returned no picture for a
// few seconds although packets were fed to it. Time is passed in, so it can be tested without a clock.
#pragma once
namespace dect2 {

class HwStartGuard {
public:
    static constexpr double kGiveUpSec = 3.0;      // seconds of input fed without a single returned picture (issue #203)
    static constexpr int kGiveUpPackets = 25;      // ...and at least this many packets fed (about half a second at 50 fps)
    static constexpr int kMaxFetchFails = 5;       // pictures decoded by the GPU that could not be copied out

    void arm(double nowSec) { armed_ = nowSec; }
    // a video packet arrived; returns true if it should be submitted to the GPU decoder.
    // `irap`: the access unit is a random-access point (HEVC CRA/IDR/BLA, or a key frame for other codecs). A RASL or
    // trailing picture with no IRAP before it must not start the session: the GPU rejects it and never recovers.
    bool feed(bool irap, double nowSec) {
        if (!started_) {
            if (!irap) { skipped_++; return false; }   // wait for an IRAP: the GPU drops everything before it anyway
            started_ = true; t0_ = nowSec;
        }
        fed_++;
        return true;
    }
    void gotPicture() { pictures_++; }
    void fetchFailed() { fetchFails_++; }
    // true when hardware decoding should be given up for this stream
    bool giveUp(double nowSec) const {
        if (pictures_ > 0) return false;
        if (fetchFails_ >= kMaxFetchFails) return true;
        if (started_) return fed_ >= kGiveUpPackets && nowSec - t0_ >= kGiveUpSec;
        // no keyframe at all for a long time (gradual-refresh streams): the CPU decoder can show those
        return skipped_ >= kGiveUpPackets && nowSec - armed_ >= 2 * kGiveUpSec;
    }
    int skipped() const { return skipped_; }
    int fed() const { return fed_; }
private:
    bool started_ = false;
    double t0_ = 0, armed_ = 0;
    int skipped_ = 0, fed_ = 0, pictures_ = 0, fetchFails_ = 0;
};

} // namespace dect2
