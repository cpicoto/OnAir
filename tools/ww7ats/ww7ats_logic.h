// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WW7ATS receiver: the pieces of the simple DVB-T screen that need no window (tested in tests/app/test_ww7ats_logic.cpp).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ww7ats {

// The WW7ATS ATV repeater on Cougar Mountain (Issaquah, WA): DVB-T, 1255 MHz, 6 MHz channel.
constexpr double kCenterHz = 1255e6;
constexpr double kBandwidthMhz = 6;
constexpr int kStandardDvbt = 2;   // Engine::setStandard code for DVB-T (dect2cli --standard t)

// One HackRF gain number (0..102 dB) split over its LNA (0..40 dB, steps of 8) and VGA (0..62 dB, steps of 2) stages. The LNA takes
// about half, since the first stage sets the noise figure; the VGA takes the rest.
struct HackrfGain { int lnaDb = 0, vgaDb = 0; };
constexpr int kHackrfGainMax = 40 + 62;

inline HackrfGain splitHackrfGain(int totalDb) {
    totalDb = std::clamp(totalDb, 0, kHackrfGainMax);
    HackrfGain g;
    g.lnaDb = std::min(40, (totalDb / 2) / 8 * 8);
    g.vgaDb = std::clamp((totalDb - g.lnaDb) / 2 * 2, 0, 62);
    return g;
}

// NV12 (Y plane + interleaved UV at half resolution) to RGBA8. bt709: HD matrix, else BT.601. fullRange: 0..255 luma.
inline void nv12ToRgba(const uint8_t* y, const uint8_t* uv, int w, int h, bool bt709, bool fullRange, std::vector<uint8_t>& out) {
    out.resize((size_t)w * h * 4);
    const float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f, kg = 1.f - kr - kb;
    const float ys = fullRange ? 1.f : 255.f / 219.f, cs = fullRange ? 1.f : 255.f / 224.f, yo = fullRange ? 0.f : 16.f;
    const float rv = 2.f * (1.f - kr) * cs, bu = 2.f * (1.f - kb) * cs;
    const float gu = -bu * kb / kg, gv = -rv * kr / kg;
    auto clamp8 = [](float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); };
    for (int j = 0; j < h; j++) {
        const uint8_t* yr = y + (size_t)j * w;
        const uint8_t* cr = uv + (size_t)(j / 2) * w;
        uint8_t* o = out.data() + (size_t)j * w * 4;
        for (int i = 0; i < w; i++) {
            const float Y = (yr[i] - yo) * ys;
            const float U = cr[i & ~1] - 128.f, V = cr[(i & ~1) + 1] - 128.f;
            o[4 * i + 0] = clamp8(Y + rv * V);
            o[4 * i + 1] = clamp8(Y + gu * U + gv * V);
            o[4 * i + 2] = clamp8(Y + bu * U);
            o[4 * i + 3] = 255;
        }
    }
}

// What the status line says, from the receiver's lock stages.
enum class Lock { Stopped, Searching, Sync, Tps, Fec, Playing };
inline Lock lockStage(bool running, int rxState, bool tpsOk, bool fecSync, bool playing) {
    if (!running) return Lock::Stopped;
    if (fecSync && playing) return Lock::Playing;
    if (fecSync) return Lock::Fec;
    if (tpsOk) return Lock::Tps;
    if (rxState >= 2) return Lock::Sync;
    return Lock::Searching;
}
inline const char* lockText(Lock l) {
    switch (l) {
    case Lock::Stopped: return "Stopped";
    case Lock::Searching: return "Searching for signal";
    case Lock::Sync: return "OFDM sync, reading TPS";
    case Lock::Tps: return "TPS locked, waiting for FEC";
    case Lock::Fec: return "Locked, waiting for a service";
    case Lock::Playing: return "Locked, playing";
    }
    return "";
}

inline const char* modName(int mod) { return mod == 0 ? "QPSK" : mod == 1 ? "16-QAM" : mod == 2 ? "64-QAM" : "?"; }
inline const char* rateName(int cr) {
    static const char* n[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
    return cr >= 0 && cr < 5 ? n[cr] : "?";
}
inline const char* guardName(int g) {
    static const char* n[] = {"1/32", "1/16", "1/8", "1/4"};
    return g >= 0 && g < 4 ? n[g] : "?";
}
inline const char* fftName(int mode) { return mode == 0 ? "2K" : mode == 1 ? "8K" : mode == 2 ? "4K" : "?"; }

}  // namespace ww7ats
