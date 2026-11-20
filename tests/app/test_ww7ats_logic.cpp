// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The WW7ATS receiver's window-free logic (tools/ww7ats/ww7ats_logic.h): the HackRF gain split stays within the stages' ranges and
// steps and adds up, NV12 grey and primaries come out right, and the lock stage follows the receiver.
#include "ww7ats_logic.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    for (int t = -5; t <= ww7ats::kHackrfGainMax + 5; t++) {
        const ww7ats::HackrfGain g = ww7ats::splitHackrfGain(t);
        const int want = t < 0 ? 0 : t > ww7ats::kHackrfGainMax ? ww7ats::kHackrfGainMax : t;
        CHECK(g.lnaDb >= 0 && g.lnaDb <= 40 && g.lnaDb % 8 == 0, "LNA %d for %d", g.lnaDb, t);
        CHECK(g.vgaDb >= 0 && g.vgaDb <= 62 && g.vgaDb % 2 == 0, "VGA %d for %d", g.vgaDb, t);
        CHECK(want - (g.lnaDb + g.vgaDb) >= 0 && want - (g.lnaDb + g.vgaDb) <= 1, "total %d+%d for %d", g.lnaDb, g.vgaDb, t);
    }
    CHECK(ww7ats::splitHackrfGain(ww7ats::kHackrfGainMax).lnaDb == 40, "full gain: full LNA");

    // 2x2 picture: limited-range grey, then pure red via UV
    const uint8_t y[4] = {126, 126, 126, 126}, uvGrey[2] = {128, 128};
    std::vector<uint8_t> out;
    ww7ats::nv12ToRgba(y, uvGrey, 2, 2, true, false, out);
    CHECK(out.size() == 16, "RGBA size");
    for (int i = 0; i < 4; i++) CHECK(std::abs(out[4 * i] - 128) <= 1 && out[4 * i] == out[4 * i + 1] && out[4 * i + 1] == out[4 * i + 2] && out[4 * i + 3] == 255, "grey pixel %d: %d %d %d", i, out[4 * i], out[4 * i + 1], out[4 * i + 2]);
    const uint8_t yRed[4] = {63, 63, 63, 63}, uvRed[2] = {102, 240};   // BT.709 limited-range red
    ww7ats::nv12ToRgba(yRed, uvRed, 2, 2, true, false, out);
    CHECK(out[0] > 245 && out[1] < 10 && out[2] < 10, "red: %d %d %d", out[0], out[1], out[2]);

    using ww7ats::Lock;
    CHECK(ww7ats::lockStage(false, 2, true, true, true) == Lock::Stopped, "stopped wins");
    CHECK(ww7ats::lockStage(true, 0, false, false, false) == Lock::Searching, "searching");
    CHECK(ww7ats::lockStage(true, 2, false, false, false) == Lock::Sync, "sync");
    CHECK(ww7ats::lockStage(true, 2, true, false, false) == Lock::Tps, "tps");
    CHECK(ww7ats::lockStage(true, 2, true, true, false) == Lock::Fec, "fec");
    CHECK(ww7ats::lockStage(true, 2, true, true, true) == Lock::Playing, "playing");

    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("ww7ats logic: ok\n");
    return 0;
}
