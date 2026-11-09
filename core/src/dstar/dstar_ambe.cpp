// AMBE through the user-installed mbelib (see dstar_ambe.h); the loader is shared with the other voice decoders (source/mbe_dyn.h).
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/dstar/dstar_proto.h"
#include "../source/mbe_dyn.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/voice/vocoder_fec.h"
#include <cstring>

namespace dect2 {

namespace {
using FrameFn = void (DECT2_CALL*)(short* aout, int* errs, int* errs2, char* errStr, char fr[4][24], char d[49], void* cur, void* prev, void* prevEnh, int uvq);
mbe::Loader<FrameFn> gMbe("mbe_processAmbe3600x2400Frame");
} // namespace

bool dstarAmbeAvailable(std::string* path) { return codec::pluginLoaded("ambe", path) || gMbe.available(path); }
bool dstarAmbeRescan() { return gMbe.rescan(); }

struct DstarAmbe::Impl {
    const mbe::Api<FrameFn>* a = nullptr;
    std::shared_ptr<mbe::State> s = std::make_shared<mbe::State>();
    std::unique_ptr<codec::SpeechCodec> plug;   // the user's codec plug-in "ambe" (docs/CODECS.md); preferred over mbelib
    voicefec::AmbeProtector prot;
    int perm[96];
    bool permOk = false;
};

DstarAmbe::DstarAmbe() : p_(std::make_unique<Impl>()) {
    p_->plug = codec::pluginVocoder("ambe");
    if (p_->plug) return;
    const mbe::Api<FrameFn>& a = gMbe.api();
    if (!a.ok) return;
    if (!mbe::initState(a, p_->s)) return;   // on the codec guard: a library that crashes is disabled, OnAir goes on
    p_->a = &a;
}
DstarAmbe::~DstarAmbe() = default;
bool DstarAmbe::ok() const { return p_->plug || p_->a != nullptr; }

void DstarAmbe::decode(const uint8_t bits[72], int16_t pcm[160]) {
    char fr[4][24];
    dstar::ambeFrame(bits, fr);
    decodeFrame(fr, pcm);
}

void DstarAmbe::decodeSoft(const uint8_t bits[72], const uint8_t rel[72], int16_t pcm[160]) {
    char fr[4][24];
    dstar::ambeFrame(bits, fr);
    if (!voicefec::enabled()) { decodeFrame(fr, pcm); return; }
    if (!p_->permOk) { voicefec::permutationOf([](const uint8_t* b, char f[4][24]) { dstar::ambeFrame(b, f); }, p_->perm); p_->permOk = true; }
    uint8_t r[4][24];
    voicefec::permuteReliability(p_->perm, rel, r);
    char out[4][24];
    const voicefec::FrameInfo fi = p_->prot.process(fr, r, out);
    decodeFrame(out, pcm);
    if (fi.gain != 1.f) for (int i = 0; i < 160; i++) pcm[i] = (int16_t)(pcm[i] * fi.gain);
}

void DstarAmbe::fecCounts(int* frames, int* repaired, int* concealed) const {
    if (frames) *frames = p_->prot.frames;
    if (repaired) *repaired = p_->prot.repairedFrames;
    if (concealed) *concealed = p_->prot.concealedFrames;
}

void DstarAmbe::decodeFrame(const char fr[4][24], int16_t pcm[160]) {
    if (!p_->a && !p_->plug) { memset(pcm, 0, 160 * sizeof(int16_t)); return; }
    if (p_->plug) {
        const std::vector<int16_t> v = p_->plug->decode(codec::packCodeWords(&fr[0][0], 4, 24));
        memset(pcm, 0, 160 * sizeof(int16_t));
        for (size_t i = 0; i < v.size() && i < 160; i++) pcm[i] = v[i];
        return;
    }
    char fc[4][24]; memcpy(fc, fr, sizeof(fc));
    mbe::decodeFrame(*p_->a, p_->s, &fc[0][0], 4 * 24, pcm);   // guarded: never on the caller's stack, a crash or hang disables the library
}

} // namespace dect2
