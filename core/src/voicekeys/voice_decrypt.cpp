// Per-call voice decryption (see voice_decrypt.h). Applies a user key's keystream to the voice-frame bits. No key search or recovery.
#include "dect2/voicekeys/voice_decrypt.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace voicekeys {

const char* decryptStatusText(DecryptStatus s) {
    switch (s) {
        case DecryptStatus::NotEncrypted: return "clear";
        case DecryptStatus::NoKey: return "encrypted: add your key";
        case DecryptStatus::Decrypted: return "decrypted";
        case DecryptStatus::WrongKey: return "key mismatch";
        case DecryptStatus::Unsupported: return "encrypted: unsupported algorithm";
    }
    return "?";
}

std::string Decryptor::statusLine(DecryptStatus s, const std::string& keyName) {
    if (s == DecryptStatus::Decrypted && !keyName.empty()) return std::string("decrypted with key ") + keyName;
    return decryptStatusText(s);
}

Decryptor::Decryptor(const VoiceKey& key, int frameBits) : key_(key), frameBits_(frameBits), alg_(key.algorithm), name_(key.name) {
    switch (alg_) {
        case Algorithm::Rc4:
            // RC4 is keyed per call from the key bytes with the IV appended (set in setIv). Keyed again when the IV arrives.
            rc4_.setKey(key_.key.data(), key_.key.size());
            break;
        case Algorithm::DesOfb:
            if (key_.key.size() == 8) des_.setKey(key_.key.data());
            else { status_ = DecryptStatus::Unsupported; note_ = "DES needs an 8-byte key"; }
            break;
        case Algorithm::Aes128Ofb:
            if (key_.key.size() != 16) { status_ = DecryptStatus::Unsupported; note_ = "AES-128 needs a 16-byte key"; }
            break;
        case Algorithm::Aes256Ofb:
            if (key_.key.size() != 32) { status_ = DecryptStatus::Unsupported; note_ = "AES-256 needs a 32-byte key"; }
            break;
        case Algorithm::Scrambler:
            lfsr_.setSeed((uint16_t)((key_.key.size() >= 2 ? (key_.key[0] << 8 | key_.key[1]) : (key_.key.empty() ? 1 : key_.key[0])) & 0x7FFF));
            break;
        case Algorithm::HyteraBp:
        case Algorithm::HyteraEnhanced:
            hyt_ = std::make_unique<HyteraDecryptor>(alg_ == Algorithm::HyteraBp ? HyteraDecryptor::Basic : HyteraDecryptor::Enhanced, key_.key);
            if (!hyt_->ok()) { status_ = DecryptStatus::Unsupported; note_ = alg_ == Algorithm::HyteraBp ? "Hytera-BP needs a 5, 16 or 32 byte key" : "Hytera-Enhanced needs a 5-byte key"; }
            else if (frameBits_ != 49) { status_ = DecryptStatus::Unsupported; note_ = "Hytera privacy works on the 49 vocoder bits of a DMR frame"; }
            break;
        default:
            status_ = DecryptStatus::Unsupported; note_ = "no algorithm";
            break;
    }
}

void Decryptor::setIv(const uint8_t* iv, size_t n) {
    std::memset(iv_, 0, sizeof iv_);
    std::memcpy(iv_, iv, std::min(n, sizeof iv_));
    haveIv_ = true;
    ofbPos_ = 16;
    std::memcpy(ofbFb_, iv_, 16);
    if (hyt_) { if (alg_ == Algorithm::HyteraEnhanced && n >= 5) hyt_->setMessageIndicator(iv_); return; }
    if (alg_ == Algorithm::Rc4) {
        // RC4 re-keyed with key || IV, as the P25 ADP profile derives its key schedule from the key and the message indicator.
        std::vector<uint8_t> seed = key_.key;
        seed.insert(seed.end(), iv_, iv_ + std::min(n, (size_t)9));
        rc4_.setKey(seed.data(), seed.size());
    } else if (alg_ == Algorithm::DesOfb) {
        // DES-OFB IV is the first 8 MI bytes.
        std::memcpy(ofbFb_, iv_, 8);
    }
}

void Decryptor::keystreamBytes(uint8_t* out, size_t n) {
    if (alg_ == Algorithm::Rc4) { rc4_.keystream(out, n); return; }
    const size_t blk = (alg_ == Algorithm::DesOfb) ? 8 : 16;
    size_t done = 0;
    while (done < n) {
        if (ofbPos_ >= blk) {
            if (alg_ == Algorithm::DesOfb) { uint8_t o[8]; des_.encryptBlock(ofbFb_, o); std::memcpy(ofbFb_, o, 8); std::memcpy(ofbBuf_, o, 8); }
            else {
                const size_t kl = (alg_ == Algorithm::Aes128Ofb) ? 16 : 32;
                uint8_t o[16]; aesOfbKeystream(key_.key.data(), kl, ofbFb_, o, 16);
                // aesOfbKeystream from ofbFb_ gives E(fb) as the first 16 bytes; feed it back.
                std::memcpy(ofbFb_, o, 16); std::memcpy(ofbBuf_, o, 16);
            }
            ofbPos_ = 0;
        }
        const size_t take = std::min(n - done, blk - ofbPos_);
        std::memcpy(out + done, ofbBuf_ + ofbPos_, take);
        ofbPos_ += take; done += take;
    }
}

bool Decryptor::decryptFrame(uint8_t* bits) {
    if (status_ == DecryptStatus::Unsupported) return false;
    const size_t nb = (size_t)frameBits_;
    if (hyt_) { hyt_->applyFrame(bits); return true; }
    if (alg_ == Algorithm::Scrambler) {
        uint8_t ks[256];
        for (size_t off = 0; off < nb; off += sizeof ks) {
            const size_t take = std::min(sizeof ks, nb - off);
            lfsr_.keystreamBits(ks, take);
            xorBits(bits + off, ks, take);
        }
        return true;
    }
    // byte-oriented keystream (RC4 / OFB): one keystream bit per voice bit, packed 8 to a byte.
    const size_t bytes = (nb + 7) / 8;
    std::vector<uint8_t> ks(bytes);
    keystreamBytes(ks.data(), bytes);
    for (size_t i = 0; i < nb; i++) bits[i] ^= (ks[i >> 3] >> (7 - (i & 7))) & 1;
    return true;
}

void Decryptor::reportFrameErrors(int errs) {
    if (status_ == DecryptStatus::Unsupported) return;
    frames_++;
    if (errs > 6) badFrames_++;    // a heavily errored vocoder frame; the threshold is advisory only
    // after a short window, decide. A good key gives mostly clean frames; a wrong key gives noise in almost every frame.
    if (frames_ >= 6) {
        status_ = (badFrames_ * 2 > frames_) ? DecryptStatus::WrongKey : DecryptStatus::Decrypted;
    } else if (status_ != DecryptStatus::WrongKey) {
        status_ = DecryptStatus::Decrypted;
    }
}

Lookup lookupForCall(const CallKeyInfo& info, bool encrypted, int frameBits, const uint8_t* iv, size_t ivLen) {
    Lookup out;
    if (!encrypted) { out.status = DecryptStatus::NotEncrypted; return out; }
    const auto cands = store().candidates(info);
    if (cands.empty()) { out.status = DecryptStatus::NoKey; return out; }
    // Pick the first candidate (explicit match, or the first of the user's ordered trial list). The receiver may rebuild with the next
    // one only when this call carried NO key id and the frames came out as noise — that stays the user's small ordered list, never a search.
    const VoiceKey& k = cands.front();
    out.decryptor = std::make_unique<Decryptor>(k, frameBits);
    if (iv && ivLen) out.decryptor->setIv(iv, ivLen);
    out.status = out.decryptor->status();
    out.keyName = k.name;
    return out;
}

} // namespace voicekeys
} // namespace dect2
