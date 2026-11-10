// DRM test signal: DRM30 transmitter (modes A to D), DRM+ (mode E), the channels of Annex B.1, and the synthetic source.
#include "dect2/drm/drm_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/drm/drm_fec.h"
#include "dect2/drm/drm_fft.h"
#include "dect2/dsp/exact_resampler.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace drm;

// ---------------------------------------------------------------- audio sources

void DrmPatternSource::pattern(uint32_t seed, uint64_t index, int numFrames, int payload, std::vector<std::vector<uint8_t>>& frames, std::vector<uint8_t>& crc) {
    std::mt19937 rng((uint32_t)(seed * 2654435761u + (uint32_t)index * 40503u + 17u));
    frames.assign((size_t)numFrames, {});
    crc.assign((size_t)numFrames, 0);
    const int base = payload / numFrames;
    int extra = payload - base * numFrames;
    for (int f = 0; f < numFrames; f++) {
        const int len = base + (f < extra ? 1 : 0);
        auto& fr = frames[(size_t)f];
        fr.resize((size_t)len);
        for (int i = 0; i < len; i++) fr[(size_t)i] = (uint8_t)rng();
        if (len >= 4) {                                   // the frame carries its number: the receiver tests use it to see lost or repeated frames
            fr[0] = (uint8_t)(index >> 8); fr[1] = (uint8_t)index; fr[2] = (uint8_t)f; fr[3] = 0xA5;
        }
        crc[(size_t)f] = (uint8_t)crc8Bytes(fr.data(), (size_t)std::min(len, 4));
    }
}

void DrmPatternSource::nextSuperFrame(int numFrames, int payload, std::vector<std::vector<uint8_t>>& frames, std::vector<uint8_t>& crc) {
    pattern(seed_, index_++, numFrames, payload, frames, crc);
}

// ---------------------------------------------------------------- transmitter

struct DrmTransmitter::Impl {
    DrmTxConfig cfg;
    std::unique_ptr<DrmAudioSource> audio;
    std::shared_ptr<const Layout> L;
    bool valid = false;
    int N = 0, G = 0;                         // useful part and guard in samples at 48 kHz (mode E: 192 kHz)
    bool modeE = false;
    int halfBytes = 0;                        // mode E: bytes of the audio super frame in each of its two logical frames
    std::vector<uint8_t> secondHalf;          // mode E: the second logical frame of the audio super frame in progress
    uint64_t muxMade = 0;                     // multiplex frames made (mode E: the even ones start an audio super frame)
    std::unique_ptr<MlcCode> mscCode, sdcCode, facCode;
    int levels = 1;
    int lenA = 0, lenB = 0, textBytes = 0, nAudio = 0, hp = 0, hdr = 0, payload = 0;
    int sdcFieldBytes = 0;
    // the data service (packet mode stream 1)
    int dataLen = 0;                          // bytes of its logical frame: whole packets
    std::vector<uint8_t> dataQ;               // the data unit being cut into packets
    size_t dataQPos = 0;
    uint8_t dataCi = 0;                       // continuity index of the packets of the data service's Packet Id
    uint64_t dataSlot = 0;
    bool hasData() const { return dataLen > 0; }
    std::vector<std::vector<cf32>> hist;      // previous multiplex frames z (before the cell interleaver), most recent first
    int depth = 1;
    TextEncoder text;
    uint64_t sf = 0;                          // super frames generated
    int nMuxFrames = 0;
    int entityRotation = 0;
    DrmFft fft;
    double scale = 1;
    std::vector<cf32> grid;
    int preroll = 0;
    int minuteOfDay0 = 0, mjd0 = 0;
    double elapsedSec = 0;

    explicit Impl(const DrmTxConfig& c, std::unique_ptr<DrmAudioSource> a) : cfg(c), audio(std::move(a)) {
        const int mode = cfg.mode;
        if (mode < 0 || mode > 4) return;
        modeE = mode == kModeE;
        if (modeE) { cfg.occupancy = 0; cfg.partABytes = 0; }   // one occupancy (100 kHz); the test signal has equal error protection
        L = layout(mode, cfg.occupancy);
        if (!L) return;
        const ModeParams& mp = modeParams(mode);
        const int spt = modeE ? 16 : 4;                    // samples per T: 48 kHz, mode E 192 kHz
        N = mp.tu12 * spt; G = mp.tg12 * spt;
        fft = DrmFft(N);
        if (modeE ? (cfg.mscQam != 4 && cfg.mscQam != 16) : (cfg.mscQam != 16 && cfg.mscQam != 64)) return;
        const int qb = cfg.mscQam == 64 ? 6 : cfg.mscQam == 16 ? 4 : 2;   // bits per cell
        levels = qb / 2;
        if (mscProtLevels(mode, qb) <= cfg.protB || cfg.protB < 0) return;
        nAudio = aacNumFrames(modeE, cfg.audioRateHz);
        if (!nAudio) return;
        hdr = aacHeaderBytes(nAudio);
        textBytes = cfg.textMessage ? 4 : 0;
        // MSC code: one audio stream, equal or unequal error protection
        MlcParams p;
        p.levels = levels;
        for (int l = 0; l < levels; l++) {
            const Rate b = mscRate(mode, qb, cfg.protB, l);
            p.rxB[l] = b.rx; p.ryB[l] = b.ry;
            const Rate a2 = mscRate(mode, qb, cfg.partABytes > 0 ? cfg.protA : cfg.protB, l);
            p.rxA[l] = a2.rx; p.ryA[l] = a2.ry;
        }
        int X = 0;
        if (cfg.partABytes > 0) {
            // part A: enough audio bytes for the higher protected blocks of all frames (header, then hp bytes and a CRC byte per frame)
            hp = std::max(0, (cfg.partABytes - hdr) / nAudio - 1);
            X = hdr + nAudio * (hp + 1);
            const int lcm = mscRyLcm(mode, qb, cfg.protA);
            int den = 0;
            for (int l = 0; l < levels; l++) den += 2 * p.rxA[l] * (lcm / p.ryA[l]);
            p.n1 = lcm * ((8 * X + den - 1) / den);
        }
        audio->setHigherProtectedBytes(hp);
        p.n2 = L->nMux - p.n1;
        if (p.n2 < 40) return;
        mscCode = std::make_unique<MlcCode>(p);
        const int capacity = mscCode->infoBits() / 8;
        lenA = X;
        lenB = capacity - X;
        if (!modeE && cfg.dataApp > 0 && cfg.dataSource && cfg.dataPackets > 0 && cfg.dataPacketLen >= 8 && cfg.dataPacketLen <= 255) {
            dataLen = cfg.dataPackets * (cfg.dataPacketLen + 3);
            lenB -= dataLen;                          // stream 1 takes the end of part B
        }
        if (lenA > 4095 || lenB > 4095 || lenB < 40) return;
        // mode E: the audio super frame (200 ms) fills two logical frames (100 ms each), half of it in each
        halfBytes = lenA + lenB - textBytes;
        payload = (modeE ? 2 * halfBytes : halfBytes) - hdr - nAudio;
        if (payload < nAudio * 8) return;
        // SDC and FAC codes (clause 7.5.1, Tables 36 to 38; mode E: 4-QAM, rate 1/2 or 1/4, and the FAC at rate 1/4)
        MlcParams s;
        s.n2 = L->nSdc;
        if (modeE) { s.levels = 1; s.rxB[0] = 1; s.ryB[0] = cfg.sdcMode == 0 ? 2 : 4; }
        else if (cfg.sdcMode == 0) { s.levels = 2; s.rxB[0] = 1; s.ryB[0] = 3; s.rxB[1] = 2; s.ryB[1] = 3; }
        else { s.levels = 1; s.rxB[0] = 1; s.ryB[0] = 2; }
        sdcCode = std::make_unique<MlcCode>(s);
        sdcFieldBytes = sdcDataBytes(mode, cfg.occupancy, cfg.sdcMode);
        MlcParams f;
        f.levels = 1; f.n2 = L->nFac; f.fac = true; f.rxB[0] = modeE ? 1 : 3; f.ryB[0] = modeE ? 4 : 5;
        facCode = std::make_unique<MlcCode>(f);
        depth = cfg.longInterleave ? mp.depth : 1;
        text.set(cfg.text);
        mjd0 = dateToMjd(cfg.year, cfg.month, cfg.day);
        minuteOfDay0 = cfg.hour * 60 + cfg.minute;
        // power normalisation: mean power of all cells per symbol
        double pw = 0;
        const int nsym = L->symbols();
        for (int s2 = 0; s2 < nsym; s2++) {
            for (const Pilot& pl : L->pilots[(size_t)s2]) pw += std::norm(pl.ref);
        }
        pw += (double)(L->nFac * L->frames + L->nSdc + L->nSfa);
        pw /= nsym;
        scale = 1.0 / std::sqrt(pw);
        valid = true;
        // pre-roll: the cell interleaver needs the previous multiplex frames
        hist.clear();
        for (int j = 0; j < depth - 1; j++) {
            std::vector<cf32> z = muxCells();
            hist.insert(hist.begin(), z);
        }
        preroll = depth - 1;
    }

    // A packet of the data service (ES 201 980 clause 6.6.1): header (first, last, packet id, padded packet indicator, continuity index), the data field, CRC-16
    void dataPacket(std::vector<uint8_t>& out) {
        const int pl = cfg.dataPacketLen;
        if (dataQPos >= dataQ.size()) { dataQ = cfg.dataSource->nextUnit(); dataQPos = 0; }
        const bool empty = dataQ.empty();
        const size_t remaining = dataQ.size() - dataQPos;
        const bool first = empty || dataQPos == 0, last = empty || remaining <= (size_t)pl - 1 || remaining == (size_t)pl;
        const bool ppi = empty || remaining < (size_t)pl;
        const size_t take = empty ? 0 : std::min(remaining, (size_t)pl - (ppi ? 1 : 0));
        std::vector<uint8_t> pk((size_t)pl + 3, 0);
        pk[0] = (uint8_t)((first ? 0x80 : 0) | (last && take == remaining ? 0x40 : 0) | ((cfg.dataPacketId & 3) << 4) | (ppi ? 0x08 : 0) | (dataCi & 7));
        size_t at = 1;
        if (ppi) pk[at++] = (uint8_t)take;
        if (take) std::memcpy(&pk[at], &dataQ[dataQPos], take);
        dataQPos += take;
        if (empty) pk[0] |= 0xC0;
        const uint32_t c = drm::crc16Bytes(pk.data(), (size_t)pl + 1);
        pk[(size_t)pl + 1] = (uint8_t)(c >> 8); pk[(size_t)pl + 2] = (uint8_t)c;
        dataCi = (uint8_t)((dataCi + 1) & 7);
        out = pk;
    }
    // The bytes of the data stream's logical frame: dataPackets packets, with the losses and damage the configuration asks for
    void dataFrame(std::vector<uint8_t>& lf) {
        int sent = 0;
        while (sent < cfg.dataPackets) {
            dataSlot++;
            if (cfg.dataOtherIdEvery > 0 && dataSlot % (uint64_t)cfg.dataOtherIdEvery == 0) {   // a packet of another application on the same stream
                std::vector<uint8_t> pk((size_t)cfg.dataPacketLen + 3, 0);
                pk[0] = (uint8_t)(0xC0 | (((cfg.dataPacketId + 1) & 3) << 4) | 0x08);   // first and last, padded, no data
                const uint32_t c = drm::crc16Bytes(pk.data(), (size_t)cfg.dataPacketLen + 1);
                pk[(size_t)cfg.dataPacketLen + 1] = (uint8_t)(c >> 8); pk[(size_t)cfg.dataPacketLen + 2] = (uint8_t)c;
                lf.insert(lf.end(), pk.begin(), pk.end()); sent++;
                continue;
            }
            std::vector<uint8_t> pk;
            dataPacket(pk);
            if (cfg.dataLossEvery > 0 && dataSlot % (uint64_t)cfg.dataLossEvery == 0) continue;   // not sent: the next packet follows with the next index
            if (cfg.dataBadCrcEvery > 0 && dataSlot % (uint64_t)cfg.dataBadCrcEvery == 0) pk.back() ^= 0x5A;
            lf.insert(lf.end(), pk.begin(), pk.end()); sent++;
        }
    }

    // the cells (before the time interleaver) of the next multiplex frame
    std::vector<cf32> muxCells() {
        const int n = nAudio;
        std::vector<uint8_t> lf;
        // the position of this multiplex frame in its super frame: the pre-roll frames come before the first super frame
        const int pos = (int)(((int64_t)muxMade - (depth - 1)) % L->frames + L->frames) % L->frames;
        muxMade++;
        if (!modeE || pos % 2 == 0 || secondHalf.empty()) {
            std::vector<std::vector<uint8_t>> frames;
            std::vector<uint8_t> crc;
            audio->nextSuperFrame(n, payload, frames, crc);
            aacSuperFrameBuild(frames, crc, hp, modeE ? 2 * halfBytes : halfBytes, lf);
            if (modeE) {
                secondHalf.assign(lf.begin() + halfBytes, lf.end());
                lf.resize((size_t)halfBytes);
                if (pos % 2 == 1) { lf = secondHalf; secondHalf.clear(); }   // a pre-roll frame that is the second half of its super frame
            }
        } else {
            lf = secondHalf;
            secondHalf.clear();
        }
        if (textBytes) {
            uint8_t four[4];
            text.next(four);
            lf.insert(lf.end(), four, four + 4);
        }
        if (hasData()) dataFrame(lf);
        const int Lbits = mscCode->infoBits();
        std::vector<uint8_t> u((size_t)Lbits, 0), pr((size_t)Lbits);
        for (size_t i = 0; i < lf.size() && i * 8 < (size_t)Lbits; i++)
            for (int j = 0; j < 8 && (int)(i * 8 + (size_t)j) < Lbits; j++) u[i * 8 + (size_t)j] = (lf[i] >> (7 - j)) & 1;
        prbs(pr.data(), pr.size());
        for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
        std::vector<cf32> z((size_t)mscCode->cells());
        mscCode->encode(u.data(), z.data());
        return z;
    }

    std::vector<cf32> muxFrame() {
        std::vector<cf32> z = muxCells();
        const int M = (int)z.size();
        std::vector<cf32> out((size_t)M);
        const std::vector<int>& pi = interleavePerm(M, 5);
        for (int i = 0; i < M; i++) {
            const int g = depth > 1 ? i % depth : 0;
            out[(size_t)i] = g == 0 ? z[(size_t)pi[(size_t)i]] : hist[(size_t)g - 1][(size_t)pi[(size_t)i]];
        }
        if (depth > 1) {
            hist.insert(hist.begin(), std::move(z));
            hist.resize((size_t)depth - 1);
        }
        return out;
    }

    void sdcField(std::vector<uint8_t>& field) {
        // entities that every block carries: multiplex description and audio information; the others rotate when the block is small
        SdcMux mx;
        mx.present = true; mx.nStreams = hasData() ? 2 : 1; mx.protA = cfg.partABytes > 0 ? cfg.protA : 0; mx.protB = cfg.protB;
        mx.stream[0].lenA = lenA; mx.stream[0].lenB = lenB;
        if (hasData()) { mx.stream[1].lenA = 0; mx.stream[1].lenB = dataLen; }
        SdcAudio a;
        a.shortId = 0; a.streamId = 0; a.coding = cfg.audioCoding; a.sbr = cfg.audioSbr; a.mode = cfg.audioMode;
        a.rateCode = cfg.audioRateHz == 12000 ? 1 : cfg.audioRateHz == 24000 ? 3 : cfg.audioRateHz == 48000 ? 5 : 3; a.text = cfg.textMessage;
        field.clear();
        sdcPutEntity(field, 0, 0, sdcEntityMux(mx));
        sdcPutEntity(field, 9, 0, sdcEntityAudio(a));
        if (hasData()) {
            SdcApp ap;
            ap.present = true; ap.shortId = 1; ap.streamId = 1; ap.packet = true; ap.domain = 0; ap.dataUnit = cfg.dataUnitMode;
            ap.packetId = cfg.dataPacketId; ap.packetLen = cfg.dataPacketLen;
            ap.appData = {(uint8_t)((cfg.dataApp >> 8) & 7), (uint8_t)(cfg.dataApp & 0xFF)};
            sdcPutEntity(field, 5, 0, sdcEntityApp(ap));
        }
        std::vector<std::vector<uint8_t>> opt;       // optional entities, each is a complete entity
        {
            std::vector<uint8_t> e;
            sdcPutEntity(e, 1, 0, sdcEntityLabel(0, cfg.label.substr(0, 16)));
            opt.push_back(e);
        }
        if (hasData()) { std::vector<uint8_t> e; sdcPutEntity(e, 1, 0, sdcEntityLabel(1, cfg.dataLabel.substr(0, 16))); opt.push_back(e); }
        if (cfg.sendLanguage) { std::vector<uint8_t> e; sdcPutEntity(e, 12, 0, sdcEntityLang(0, cfg.language3, cfg.country2)); opt.push_back(e); }
        if (cfg.sendTime) {
            SdcTime t;
            const long minutes = (long)minuteOfDay0 + (long)(elapsedSec / 60.0);
            t.mjd = mjd0 + (int)(minutes / 1440); t.hour = (int)((minutes % 1440) / 60); t.minute = (int)(minutes % 60);
            std::vector<uint8_t> e;
            sdcPutEntity(e, 8, 0, sdcEntityTime(t));
            opt.push_back(e);
        }
        for (const auto& x : cfg.extraEntities) { std::vector<uint8_t> e; sdcPutEntity(e, x.first, 0, x.second); opt.push_back(e); }
        for (size_t i = 0; i < opt.size(); i++) {
            const auto& e = opt[((size_t)entityRotation + i) % opt.size()];
            if ((int)(field.size() + e.size()) <= sdcFieldBytes) field.insert(field.end(), e.begin(), e.end());
        }
        if (cfg.sdcRotate) entityRotation++;
    }

    void superFrame(std::vector<cf32>& out) {
        const ModeParams& mp = modeParams(cfg.mode);
        const int W = L->width(), nsym = L->symbols();
        grid.assign((size_t)nsym * (size_t)W, cf32(0, 0));
        for (int s = 0; s < nsym; s++)
            for (const Pilot& pl : L->pilots[(size_t)s]) grid[(size_t)s * (size_t)W + (size_t)(pl.k - L->kmin)] = pl.ref;
        // FAC: one block per transmission frame
        for (int f = 0; f < L->frames; f++) {
            FacInfo fi;
            fi.identity = modeE ? f : f == 0 ? 0 : f == 1 ? 1 : 2;   // mode E: four frames, 00 .. 11
            fi.modeE = modeE;
            fi.occupancy = cfg.occupancy;
            fi.interleaver = cfg.longInterleave ? 0 : 1;
            fi.mscMode = modeE ? (cfg.mscQam == 16 ? 0 : 3) : (cfg.mscQam == 64 ? 0 : 3);
            fi.sdcMode = cfg.sdcMode;
            fi.numServicesCode = hasData() ? 5 : 4;      // one audio service (and one data service)
            fi.svc[0].id = cfg.serviceId; fi.svc[0].shortId = 0; fi.svc[0].language = cfg.language; fi.svc[0].descriptor = cfg.programmeType;
            fi.svc[1] = fi.svc[0];                       // mode E carries two sets of service parameters: with one service both describe it
            if (hasData() && f % 2 == 1) {               // the blocks of a super frame describe the services in turn
                fi.svc[0].id = cfg.serviceId + 1; fi.svc[0].shortId = 1; fi.svc[0].data = true; fi.svc[0].descriptor = 0; fi.svc[0].language = 0;
            }
            std::vector<uint8_t> bits((size_t)facInfoBits(modeE)), pr(bits.size());
            facBuild(fi, bits.data());
            prbs(pr.data(), pr.size());
            for (size_t i = 0; i < bits.size(); i++) bits[i] ^= pr[i];
            std::vector<cf32> cells((size_t)L->nFac);
            facCode->encode(bits.data(), cells.data());
            for (int i = 0; i < L->nFac; i++) {
                const auto& c = L->facCells[(size_t)i];
                grid[(size_t)(f * mp.ns + c.first) * (size_t)W + (size_t)(c.second - L->kmin)] = cells[(size_t)i];
            }
        }
        // SDC: once per super frame
        {
            std::vector<uint8_t> field, bits;
            sdcField(field);
            sdcBuildBits(0, field, sdcFieldBytes, bits);
            bits.resize((size_t)sdcCode->infoBits(), 0);
            std::vector<uint8_t> pr(bits.size());
            prbs(pr.data(), pr.size());
            for (size_t i = 0; i < bits.size(); i++) bits[i] ^= pr[i];
            std::vector<cf32> cells((size_t)L->nSdc);
            sdcCode->encode(bits.data(), cells.data());
            for (int i = 0; i < L->nSdc; i++) {
                const auto& c = L->sdcCells[(size_t)i];
                grid[(size_t)c.first * (size_t)W + (size_t)(c.second - L->kmin)] = cells[(size_t)i];
            }
        }
        // MSC: three multiplex frames and the dummy cells of Table 46
        {
            std::vector<cf32> seq;
            seq.reserve((size_t)L->nSfa);
            for (int q = 0; q < L->frames; q++) {
                std::vector<cf32> zh = muxFrame();
                seq.insert(seq.end(), zh.begin(), zh.end());
            }
            const float a = qamNorm(levels);
            const int dummies = L->nSfa - (int)seq.size();
            if (dummies >= 1) seq.push_back(cf32(a, a));
            if (dummies >= 2) seq.push_back(cf32(a, -a));
            for (int i = 0; i < L->nSfa; i++) {
                const auto& c = L->mscCells[(size_t)i];
                grid[(size_t)c.first * (size_t)W + (size_t)(c.second - L->kmin)] = seq[(size_t)i];
            }
        }
        // OFDM: one inverse FFT per symbol and a cyclic prefix
        std::vector<cf32> bins((size_t)N);
        const size_t base = out.size();
        out.resize(base + (size_t)nsym * (size_t)(N + G));
        for (int s = 0; s < nsym; s++) {
            std::fill(bins.begin(), bins.end(), cf32(0, 0));
            for (int k = L->kmin; k <= L->kmax; k++) bins[(size_t)((k + N) % N)] = grid[(size_t)s * (size_t)W + (size_t)(k - L->kmin)];
            fft.inverse(bins.data());
            cf32* o = out.data() + base + (size_t)s * (size_t)(N + G);
            const float sc = (float)scale;
            for (int i = 0; i < G; i++) o[i] = bins[(size_t)(N - G + i)] * sc;
            for (int i = 0; i < N; i++) o[G + i] = bins[(size_t)i] * sc;
        }
        sf++;
        elapsedSec += L->frames * mp.frameMs() / 1000.0;
    }
};

DrmTransmitter::DrmTransmitter(const DrmTxConfig& cfg, std::unique_ptr<DrmAudioSource> audio) {
    if (!audio) audio = makeDrmAacSource(cfg, 2);
    if (!audio) audio = std::make_unique<DrmPatternSource>(cfg.seed);
    p_ = std::make_unique<Impl>(cfg, std::move(audio));
}
DrmTransmitter::~DrmTransmitter() = default;
bool DrmTransmitter::ok() const { return p_->valid; }
const DrmTxConfig& DrmTransmitter::config() const { return p_->cfg; }
int DrmTransmitter::superFrameSamples() const { return p_->valid ? p_->L->symbols() * (p_->N + p_->G) : 0; }
int DrmTransmitter::frameSamples() const { return p_->valid ? p_->L->ns * (p_->N + p_->G) : 0; }
void DrmTransmitter::superFrame(std::vector<cf32>& out) { if (p_->valid) p_->superFrame(out); }
int DrmTransmitter::streamBytes() const { return p_->lenA + p_->lenB; }
int DrmTransmitter::streamBytesA() const { return p_->lenA; }
int DrmTransmitter::audioPayload() const { return p_->payload; }
int DrmTransmitter::audioFrames() const { return p_->nAudio; }
int DrmTransmitter::muxCells() const { return p_->L ? p_->L->nMux : 0; }
int DrmTransmitter::nSuperFrames() const { return (int)p_->sf; }
int DrmTransmitter::sampleRate() const { return p_->modeE ? 192000 : kRate; }
int DrmTransmitter::firstDecodableFrame() const { return p_->preroll; }
double DrmTransmitter::signalPower() const { return 1.0; }
double DrmTransmitter::noiseBandwidthHz() const { return p_->L ? occupancyKhz(p_->cfg.mode, p_->cfg.occupancy) * 1000.0 : 10000.0; }
double DrmTransmitter::centreOffsetHz() const {
    if (!p_->L) return 0;
    return 0.5 * (p_->L->kmin + p_->L->kmax) * modeParams(p_->cfg.mode).spacingHz();
}

// ---------------------------------------------------------------- channels of Annex B.1

namespace {
struct PathSpec { double delayMs, gain, shiftHz, spreadHz; bool fixed; };
struct ChannelSpec { int n; PathSpec p[4]; };
const ChannelSpec kChannels[7] = {
    {0, {}},
    {1, {{0, 1, 0, 0, true}}},                                                                              // 1: AWGN
    {2, {{0, 1, 0, 0, true}, {1.0, 0.5, 0, 0.1, false}}},                                                   // 2: Rice with delay
    {3, {{0, 1, 0.1, 0.1, false}, {0.7, 0.7, 0.2, 0.5, false}, {1.5, 0.5, 0.5, 1.0, false}}},               // 3: US consortium
    {2, {{0, 1, 0, 1.0, false}, {2.0, 1, 0, 1.0, false}}},                                                  // 4: CCIR poor
    {2, {{0, 1, 0, 2.0, false}, {4.0, 1, 0, 2.0, false}}},                                                  // 5
    {4, {{0, 0.5, 0, 0.1, false}, {2.0, 1, 1.2, 2.4, false}, {4.0, 0.25, 2.4, 4.8, false}, {6.0, 0.0625, 3.6, 7.2, false}}},   // 6
};

// a complex Gaussian process with a Gaussian Doppler spectrum (two sided width Dsp = 2 sigma, containing 68 % of the power), unit variance
class FadingTap {
public:
    FadingTap() = default;
    FadingTap(double spreadHz, double shiftHz, bool fixed, uint32_t seed) : fixed_(fixed), shift_(shiftHz), rng_(seed) {
        if (fixed_) return;
        const double sigma = spreadHz / 2;
        rate_ = std::max(10.0, 16.0 * spreadHz);
        // amplitude response exp(-f^2 / (4 sigma^2)) <-> h(t) = exp(-4 pi^2 sigma^2 t^2)
        const double st = 1.0 / (2.0 * M_PI * std::sqrt(2.0) * sigma);
        const int half = (int)std::ceil(4.0 * st * rate_);
        h_.resize((size_t)(2 * half + 1));
        double e = 0;
        for (int i = -half; i <= half; i++) { const double t = i / rate_; h_[(size_t)(i + half)] = std::exp(-0.5 * (t / st) * (t / st)); e += h_[(size_t)(i + half)] * h_[(size_t)(i + half)]; }
        for (auto& v : h_) v /= std::sqrt(e);
        noise_.assign(h_.size(), cf32(0, 0));
        for (auto& v : noise_) v = gauss();
        prev_ = cur_ = next_ = cf32(0, 0);
        cur_ = step();
        next_ = step();
        prev_ = cur_;
    }
    // value of the process at 48 kHz sample index n (called with consecutive n)
    cf32 at(double tSec) {
        cf32 v(1, 0);
        if (!fixed_) {
            const double pos = tSec * rate_;
            while (pos >= (double)lo_ + 1.0) { prev_ = cur_; cur_ = next_; next_ = step(); lo_++; }
            const float fr = (float)(pos - (double)lo_);
            // linear interpolation between the low rate samples cur_ (at lo_) and next_ (at lo_ + 1)
            v = cur_ + (next_ - cur_) * fr;
        }
        if (shift_ != 0) v *= cf32(std::polar(1.0, std::fmod(2 * M_PI * shift_ * tSec, 2 * M_PI)));
        return v;
    }
private:
    cf32 gauss() { dect2::normal_distribution<float> nd(0.f, dect2::math::k1OverSqrt2f); return cf32{nd(rng_), nd(rng_)}; }
    cf32 step() {
        noise_[(size_t)head_] = gauss();
        head_ = (head_ + 1) % (int)noise_.size();
        cf32 acc(0, 0);
        const int n = (int)noise_.size();
        for (int i = 0; i < n; i++) acc += noise_[(size_t)((head_ + i) % n)] * (float)h_[(size_t)i];
        return acc;
    }
    bool fixed_ = true;
    double shift_ = 0, rate_ = 10;
    std::vector<double> h_;
    std::vector<cf32> noise_;
    int head_ = 0;
    long lo_ = 0;
    cf32 prev_, cur_, next_;
    std::mt19937 rng_;
};
} // namespace

struct DrmChannelSim::Impl {
    int n = 0;
    FadingTap tap[4];
    int delay[4] = {};
    double gain[4] = {};
    double norm = 1;
    std::vector<cf32> hist;
    size_t pos = 0;
    uint64_t sample = 0;
};

DrmChannelSim::DrmChannelSim(int profile, uint32_t seed) : p_(std::make_unique<Impl>()) {
    profile = std::max(1, std::min(6, profile));
    const ChannelSpec& c = kChannels[profile];
    p_->n = c.n;
    double pw = 0;
    int maxd = 0;
    for (int i = 0; i < c.n; i++) {
        p_->tap[i] = FadingTap(c.p[i].spreadHz, c.p[i].shiftHz, c.p[i].fixed, seed * 7919u + (uint32_t)i * 104729u + 5u);
        p_->delay[i] = (int)std::lround(c.p[i].delayMs * 48.0);
        p_->gain[i] = c.p[i].gain;
        pw += c.p[i].gain * c.p[i].gain;
        maxd = std::max(maxd, p_->delay[i]);
    }
    p_->norm = 1.0 / std::sqrt(pw);
    p_->hist.assign((size_t)maxd + 1, cf32(0, 0));
}
DrmChannelSim::~DrmChannelSim() = default;

void DrmChannelSim::process(cf32* x, size_t n) {
    Impl& d = *p_;
    const size_t H = d.hist.size();
    for (size_t i = 0; i < n; i++) {
        d.hist[d.pos] = x[i];
        const double t = (double)d.sample / 48000.0;
        cf32 y(0, 0);
        for (int k = 0; k < d.n; k++) {
            const cf32 v = d.hist[(d.pos + H - (size_t)d.delay[k]) % H];
            y += v * d.tap[k].at(t) * (float)d.gain[k];
        }
        x[i] = y * (float)d.norm;
        d.pos = (d.pos + 1) % H;
        d.sample++;
    }
}

// ---------------------------------------------------------------- synthetic source

DrmTxConfig drmTxConfigFromSynth(const SynthConfig& sc) {
    DrmTxConfig tc;
    const int m = sc.modeOpt[0];
    tc.mode = m == 0 ? kModeB : std::max(0, std::min(4, m - 1));
    const int oc = sc.modeOpt[1];
    tc.occupancy = tc.mode == kModeE ? 0 : oc > 0 ? oc - 1 : 3;
    if (tc.mode == kModeE) {   // DRM+: 16-QAM or 4-QAM, protection level 2 (16-QAM, rate 1/3 and 2/3) or 1 (4-QAM, rate 1/3) by default
        tc.mscQam = sc.modeOpt[2] == 1 ? 4 : 16;
        tc.protB = sc.modeOpt[3] == 0 ? (tc.mscQam == 16 ? 2 : 1) : sc.modeOpt[3] - 1;
        tc.sdcMode = 0;
        tc.label = "OnAir DRM+";
    } else {
        tc.mscQam = sc.modeOpt[2] == 1 ? 16 : 64;
        tc.protB = sc.modeOpt[3] == 0 ? (tc.mscQam == 16 ? 0 : 1) : sc.modeOpt[3] - 1;
    }
    // 16-QAM in modes A to D has protection levels 0 and 1 only (Table 61): a higher pick takes the most robust one there is
    const int levels = mscProtLevels(tc.mode, tc.mscQam == 64 ? 6 : tc.mscQam == 16 ? 4 : 2);
    if (levels > 0) tc.protB = std::max(0, std::min(levels - 1, tc.protB));
    tc.longInterleave = sc.modeOpt[4] == 0;
    tc.textMessage = sc.modeOpt[7] == 0;
    return tc;
}

std::unique_ptr<DrmAudioSource> drmAudioFromSynth(const SynthConfig& sc, const DrmTxConfig& tc) {
    const int au = sc.modeOpt[5];
    if (au >= 0 && au <= 3) return makeDrmAacSource(tc, au);
    if (au == 4) return std::make_unique<DrmPatternSource>(tc.seed);
    return nullptr;
}


namespace {
class DrmSynth : public ModeSynth {
public:
    DrmSynth(const SynthConfig& sc, double rate) : DrmSynth(drmTxConfigFromSynth(sc), sc, rate) {}
    DrmSynth(DrmTxConfig tc, const SynthConfig& sc, double rate) : sc_(sc), rate_(rate), rng_(4242) {
        tx_ = std::make_unique<DrmTransmitter>(tc, drmAudioFromSynth(sc, tc));
        if (!tx_->ok()) {   // a combination with no room for the audio: the usual signal (mode B, 10 kHz, 64-QAM, protection 1) instead
            const std::string label = tc.label, text = tc.text;
            tc = DrmTxConfig{};
            tc.mode = kModeB; tc.occupancy = 3; tc.mscQam = 64; tc.protB = 1; tc.label = label; tc.text = text;
            tx_ = std::make_unique<DrmTransmitter>(tc, nullptr);
        }
        centre_ = tx_->centreOffsetHz();
        br_ = tx_->sampleRate();                 // 48 kHz, mode E 192 kHz
        // the channels of Annex B.1 are short wave channels (48 kHz): not for DRM+
        if (sc.modeOpt[6] > 0 && br_ == 48000) chan_ = std::make_unique<DrmChannelSim>(std::min(6, sc.modeOpt[6]), 11);
        if (br_ != 192000) up_.configure(br_, 192000.0);
        const double snr = std::pow(10.0, sc.snrDb / 10.0);
        noiseVar_ = sc.snrDb >= 100 ? 0.0 : (br_ / tx_->noiseBandwidthHz()) / snr;
        if (sc.snrDb >= 100) {   // the transmitter's clean signal (TxSession): a channel filter, so that the OFDM sidelobes (about -38 dB two channels out) stay in the channel
            constexpr int kN = 191;
            const double fc = 0.5 * tx_->noiseBandwidthHz() * 1.12 / br_, beta = 7.0, c = (kN - 1) * 0.5;
            auto i0 = [](double x) { double sum = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); sum += t; } return sum; };
            chTaps_.resize(kN);
            double sum = 0;
            for (int k = 0; k < kN; k++) {
                const double t = k - c, r = t / c;
                const double h = (t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t)) * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
                chTaps_[(size_t)k] = (float)h;
                sum += h;
            }
            for (auto& h : chTaps_) h = (float)(h / sum);
            chHist_.assign(2 * (size_t)kN, cf32(0, 0));
        }
        echoAmp_ = sc.echoDb > 0 ? (float)std::pow(10.0, -sc.echoDb / 20.0) : 0.f;
        echoDelay_ = std::max(1, std::min(8000, sc.echoDelay)) * (int)(br_ / 48000);   // echoDelay counts samples at 48 kHz
        echoBuf_.assign((size_t)echoDelay_ + 1, cf32(0, 0));
        // total power: signal 1 (the channel keeps it) + echo + noise; the output has an rms of 0.2
        const double sig = 1.0 + (double)echoAmp_ * echoAmp_;
        scale_ = (float)(0.2 / std::sqrt(sig + noiseVar_));
        step_ = 192000.0 / rate / (1.0 + sc.sroPpm * 1e-6);
        buf_.assign(4, cf32(0, 0));
        pos_ = 1.0;
    }
    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        constexpr size_t kBlk = 512;
        for (size_t o = 0; o < n; o += kBlk) {
            const size_t nb = std::min(kBlk, n - o);
            // the 192 kHz stream this block reads, made in one go
            while ((size_t)(pos_ + step_ * (double)(nb - 1)) + 3 >= buf_.size()) refill();
            const float* b = reinterpret_cast<const float*>(buf_.data());
            float* d = reinterpret_cast<float*>(out + o);
            for (size_t i = 0; i < nb; i++) {
                const double pos = pos_ + step_ * (double)i;
                const size_t i0 = (size_t)pos;
                const float t = (float)(pos - (double)i0);
                // Catmull-Rom interpolation of the 192 kHz stream (the signal is far narrower than the band, so images do not matter), real and imaginary part alike
                const float* p = b + 2 * (i0 - 1);
                for (int c = 0; c < 2; c++) {
                    const float p0 = p[c], p1 = p[2 + c], p2 = p[4 + c], p3 = p[6 + c];
                    d[2 * i + (size_t)c] = scale_ * (p1 + 0.5f * t * ((p2 - p0) + t * ((2.f * p0 - 5.f * p1 + 4.f * p2 - p3) + t * (3.f * (p1 - p2) + p3 - p0))));
                }
            }
            pos_ += step_ * (double)nb;
        }
        // drop what is behind us
        const size_t keep = (size_t)pos_ - 1;
        if (keep > 200000) { buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)keep); pos_ -= (double)keep; }
    }

private:
    void refill() {
        std::vector<cf32> s;
        tx_->superFrame(s);
        if (s.empty()) s.assign(9600, cf32(0, 0));   // no transmitter: silence, so that generate() always moves on (it froze the app before)
        // centre the occupied carriers on 0 Hz
        {
            // by a rotating phasor (renormalised every 1024 samples) instead of a sine and cosine for every sample
            const double w = -2 * M_PI * centre_ / br_;
            const float wr = (float)std::cos(w), wi = (float)std::sin(w);
            float zr = (float)std::cos(cphase_), zi = (float)std::sin(cphase_);
            for (size_t i = 0; i < s.size(); i++) {
                const float re = s[i].real(), im = s[i].imag();
                s[i] = cf32(re * zr - im * zi, re * zi + im * zr);
                const float t = zr * wr - zi * wi;
                zi = zr * wi + zi * wr; zr = t;
                if ((i & 1023) == 1023) { const float m = 1.f / std::sqrt(zr * zr + zi * zi); zr *= m; zi *= m; }
            }
            cphase_ += w * (double)s.size();
            cphase_ = std::fmod(cphase_, 2 * M_PI);
        }
        if (!chTaps_.empty()) {   // the channel filter (clean signal only): a ring of the last taps, kept twice so that one dot product reads it straight
            const size_t nt = chTaps_.size();
            for (auto& v : s) {
                chPos_ = chPos_ == 0 ? nt - 1 : chPos_ - 1;
                chHist_[chPos_] = v; chHist_[chPos_ + nt] = v;
                const cf32* x = &chHist_[chPos_];
                float re = 0, im = 0;
                for (size_t k = 0; k < nt; k++) { re += chTaps_[k] * x[k].real(); im += chTaps_[k] * x[k].imag(); }
                v = cf32(re, im);
            }
        }
        if (chan_) chan_->process(s.data(), s.size());
        if (echoAmp_ > 0) {
            for (size_t i = 0; i < s.size(); i++) {
                const cf32 d = echoBuf_[echoPos_];
                echoBuf_[echoPos_] = s[i];
                echoPos_ = (echoPos_ + 1) % echoBuf_.size();
                s[i] += d * echoAmp_;
            }
        }
        if (noiseVar_ > 0) {
            dect2::normal_distribution<float> nd(0.f, (float)std::sqrt(noiseVar_ / 2));
            for (auto& v : s) v += cf32{nd(rng_), nd(rng_)};
        }
        std::vector<cf32> u;
        if (br_ != 192000) up_.process(s.data(), s.size(), u);
        else u.swap(s);
        // carrier offset at 192 kHz
        if (sc_.cfoHz != 0 || phase_ != 0) {
            const double w = 2 * M_PI * sc_.cfoHz / 192000.0;
            const float wr = (float)std::cos(w), wi = (float)std::sin(w);
            float zr = (float)std::cos(phase_), zi = (float)std::sin(phase_);
            for (size_t i = 0; i < u.size(); i++) {
                const float re = u[i].real(), im = u[i].imag();
                u[i] = cf32(re * zr - im * zi, re * zi + im * zr);
                const float t = zr * wr - zi * wi;
                zi = zr * wi + zi * wr; zr = t;
                if ((i & 1023) == 1023) { const float m = 1.f / std::sqrt(zr * zr + zi * zi); zr *= m; zi *= m; }
            }
            phase_ = std::fmod(phase_ + w * (double)u.size(), 2 * M_PI);
        }
        buf_.insert(buf_.end(), u.begin(), u.end());
    }

    SynthConfig sc_;
    double rate_;
    std::mt19937 rng_;
    std::unique_ptr<DrmTransmitter> tx_;
    std::unique_ptr<DrmChannelSim> chan_;
    ExactResampler up_;
    double centre_ = 0, noiseVar_ = 0, step_ = 1, pos_ = 1, phase_ = 0, br_ = 48000;
    double cphase_ = 0;
    std::vector<float> chTaps_;
    std::vector<cf32> chHist_;
    size_t chPos_ = 0;
    float echoAmp_ = 0, scale_ = 0.2f;
    int echoDelay_ = 300;
    std::vector<cf32> echoBuf_;
    size_t echoPos_ = 0;
    std::vector<cf32> buf_;
};
} // namespace

std::unique_ptr<ModeSynth> makeDrmSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 48000.0 || sampleRate > 40e6) return nullptr;
    return std::make_unique<DrmSynth>(cfg, sampleRate);
}

std::unique_ptr<ModeSynth> makeDrmSynth(const DrmTxConfig& tc, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 48000.0 || sampleRate > 40e6) return nullptr;
    return std::make_unique<DrmSynth>(tc, cfg, sampleRate);
}

} // namespace dect2
