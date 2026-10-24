// AMBE through the user-installed mbelib (see nxdn_ambe.h); the loader is shared with the other voice decoders (source/mbe_dyn.h).
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "../source/mbe_dyn.h"
#include "dect2/codec/codec_registry.h"
#include <algorithm>
#include <cstring>

namespace dect2 {

namespace {
using FrameFn = void (DECT2_CALL*)(short* aout, int* errs, int* errs2, char* errStr, char fr[4][24], char d[49], void* cur, void* prev, void* prevEnh, int uvq);
mbe::Loader<FrameFn> gMbe("mbe_processAmbe3600x2450Frame");
} // namespace

bool nxdnAmbeAvailable(std::string* path) { return codec::pluginLoaded("ambe2", path) || gMbe.available(path); }
bool nxdnAmbeRescan() { return gMbe.rescan(); }

struct NxdnAmbe::Impl {
    const mbe::Api<FrameFn>* a = nullptr;
    std::shared_ptr<mbe::State> s = std::make_shared<mbe::State>();
    std::unique_ptr<codec::SpeechCodec> plug;   // the user's codec plug-in "ambe2" (docs/CODECS.md); preferred over mbelib
};

NxdnAmbe::NxdnAmbe() : p_(std::make_unique<Impl>()) {
    p_->plug = codec::pluginVocoder("ambe2");
    if (p_->plug) return;
    const mbe::Api<FrameFn>& a = gMbe.api();
    if (!a.ok) return;
    if (!mbe::initState(a, p_->s)) return;   // on the codec guard: a library that crashes is disabled, OnAir goes on
    p_->a = &a;
}
NxdnAmbe::~NxdnAmbe() = default;
bool NxdnAmbe::ok() const { return p_->plug || p_->a != nullptr; }

void NxdnAmbe::decode(const uint8_t bits[72], int16_t pcm[160]) {
    char fr[4][24];
    nxdn::ambeFrame(bits, fr);
    decodeFr(fr, pcm);
}

void NxdnAmbe::decodeFr(const char frIn[4][24], int16_t pcm[160]) {
    if (!p_->a && !p_->plug) { memset(pcm, 0, 160 * sizeof(int16_t)); return; }
    char fr[4][24];
    std::copy_n(&frIn[0][0], 4 * 24, &fr[0][0]);
    if (p_->plug) {
        const std::vector<int16_t> v = p_->plug->decode(codec::packCodeWords(&fr[0][0], 4, 24));
        memset(pcm, 0, 160 * sizeof(int16_t));
        for (size_t i = 0; i < v.size() && i < 160; i++) pcm[i] = v[i];
        return;
    }
    mbe::decodeFrame(*p_->a, p_->s, &fr[0][0], 4 * 24, pcm);   // guarded: never on the caller's stack, a crash or hang disables the library
}

} // namespace dect2
