// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WW7ATS receiver: a one-screen DVB-T receiver for the WW7ATS amateur TV repeater on Cougar Mountain (1255 MHz, 6 MHz channel).
// The OnAir engine does the work; this window only has a radio picker, Start / Stop, gain, volume, the picture and a constellation.
// GLFW + Dear ImGui on OpenGL 3, so the same code runs on macOS and Windows (and Linux). See docs/WW7ATS.md.

#include "ww7ats_logic.h"
#include "dect2/common/platform.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/source/source.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#ifdef _WIN32
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

using namespace dect2;

namespace {

struct Video {
    GLuint tex = 0;
    int w = 0, h = 0;
    double dar = 0;
    uint64_t seq = 0;
    std::vector<uint8_t> rgba;

    void upload(const VideoFrame& f) {
        const uint8_t* px = nullptr;
        if (!f.rgba.empty()) px = f.rgba.data();
        else if (!f.y.empty() && f.uv.size() >= (size_t)f.w * (f.h / 2)) {
            ww7ats::nv12ToRgba(f.y.data(), f.uv.data(), f.w, f.h, f.bt709, f.fullRange, rgba);
            px = rgba.data();
        }
        if (!px || f.w <= 0 || f.h <= 0) return;
        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.w, f.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        w = f.w; h = f.h; dar = f.dar;
    }
    void clear() { w = h = 0; }
    ~Video() { if (tex) glDeleteTextures(1, &tex); }
};

struct App {
    Engine engine;
    std::vector<DeviceInfo> radios;
    std::string radioErr;
    int radioIdx = 0;
    TuneSettings tune;
    int hackrfGain = 56;        // one number for the HackRF's LNA + VGA
    float genericGain = 30;     // dB, other radios
    bool amp = false;
    float volume = 0.7f;
    bool muted = false;
    bool gainDirty = false;
    std::chrono::steady_clock::time_point lastRetune{};
    std::string message;        // last start error
    RxTelemetry rx;
    uint64_t rxSeq = 0;
    int serviceId = -1;
    bool userPickedService = false;
    Video video;
    bool showAbout = false;

    const DeviceInfo* radio() const { return radioIdx >= 0 && radioIdx < (int)radios.size() ? &radios[radioIdx] : nullptr; }

    void refreshRadios() {
        radioErr.clear();
        radios = listRadios(radioErr);
        radios.erase(std::remove_if(radios.begin(), radios.end(), [](const DeviceInfo& d) { return !d.isRadio() || d.kind == DeviceInfo::Hamlib || d.kind == DeviceInfo::Group; }), radios.end());
        radioIdx = radios.empty() ? -1 : 0;
        if (const DeviceInfo* d = radio(); d && d->isGeneric()) genericGain = (float)std::max(d->gainMinDb, d->gainMaxDb * 0.6);
    }

    void applyGain() {
        const DeviceInfo* d = radio();
        if (d && d->kind == DeviceInfo::HackRF) {
            const ww7ats::HackrfGain g = ww7ats::splitHackrfGain(hackrfGain);
            tune.lnaDb = g.lnaDb; tune.vgaDb = g.vgaDb; tune.ampOn = amp;
        } else {
            tune.gainDb = genericGain;
        }
    }

    void start() {
        const DeviceInfo* d = radio();
        if (!d) { message = "No radio found. Connect a HackRF or another SDR and press Refresh."; return; }
        tune = TuneSettings{};
        tune.centerHz = ww7ats::kCenterHz;
        tune.bandwidthMhz = ww7ats::kBandwidthMhz;
        tune.sampleRate = 8e6;   // as the OnAir CLI uses for 6 MHz DVB-T: exact HackRF tuning at <= 10 Msps
        if (d->isGeneric()) tune.sampleRate = linkRateFor(*d, tune, tune.sampleRate, minSampleRateFor(ww7ats::kStandardDvbt, tune.bandwidthMhz));
        else if (d->maxRateHz > 0) tune.sampleRate = std::min(tune.sampleRate, d->maxRateHz);
        applyGain();
        engine.setStandard(ww7ats::kStandardDvbt);
        engine.setBandwidthAuto(false);
        engine.setSpectrumEnabled(false);
        FileOptions file;
        if (!engine.start(*d, tune, file)) {
            message = engine.startError();
            if (message.empty()) message = "The radio did not start.";
            return;
        }
        message.clear();
        engine.player().setVolume(volume);
        engine.player().setMuted(muted);
        rxSeq = 0; rx = RxTelemetry{};
        serviceId = -1; userPickedService = false;
        video.clear();
    }

    void stop() {
        engine.player().select(-1);
        engine.stop();
        serviceId = -1;
        video.clear();
    }

    void poll() {
        if (!engine.running()) return;
        if (engine.latestRx(rx, rxSeq)) rxSeq = rx.seq;
        if (gainDirty && std::chrono::steady_clock::now() - lastRetune > std::chrono::milliseconds(150)) {
            applyGain();
            engine.retune(tune);
            gainDirty = false;
            lastRetune = std::chrono::steady_clock::now();
        }
        if (!userPickedService && engine.player().selected() < 0) {
            for (auto& sv : engine.tsSnapshot().services)
                if (sv.havePmt) { engine.player().select(sv.id); serviceId = sv.id; break; }
        }
        uint64_t seq = video.seq;
        if (auto f = engine.player().videoFrame(seq); f && seq != video.seq) { video.seq = seq; video.upload(*f); }
    }
};

void constellation(const App& a, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size, p0.y + size);
    ImGui::InvisibleButton("##const", ImVec2(size, size));
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 20, 255), 4);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), IM_COL32(60, 64, 80, 255));
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), IM_COL32(60, 64, 80, 255));

    // ideal points of the signalled modulation, to scale the plot and as a reference grid
    std::vector<cf32> ideal;
    const bool tps = a.rx.standard == 1 && a.rx.dvbt.tpsOk;
    if (tps) dvbt::constellation(a.rx.dvbt.mod, a.rx.dvbt.hier, ideal);
    float range = 1.5f;
    for (auto& q : ideal) range = std::max(range, 1.25f * std::max(std::fabs(q.real()), std::fabs(q.imag())));
    const float k = size * 0.5f / range;
    for (auto& q : ideal) dl->AddCircle(ImVec2(c.x + q.real() * k, c.y - q.imag() * k), 3.5f, IM_COL32(255, 200, 60, 120));

    dl->PushClipRect(p0, p1, true);
    const ImU32 col = a.rx.dvbt.fecSync ? IM_COL32(90, 220, 140, 200) : IM_COL32(120, 170, 255, 170);
    for (auto& q : a.rx.eqData) {
        const ImVec2 p(c.x + q.real() * k, c.y - q.imag() * k);
        dl->AddRectFilled(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + 1, p.y + 1), col);
    }
    dl->PopClipRect();
    if (a.rx.eqData.empty()) {
        const char* t = a.engine.running() ? "waiting for data cells" : "not running";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + 8), IM_COL32(150, 150, 160, 255), t);
    }
}

void about(App& a) {
    if (a.showAbout) { ImGui::OpenPopup("About"); a.showAbout = false; }
    if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("WW7ATS receiver - DVB-T at 1255 MHz, 6 MHz");
        ImGui::Separator();
        ImGui::TextUnformatted("Based on OnAir by Kristijonas Adomaitis - https://on-air.dev");
        ImGui::TextUnformatted("OnAir contributors: https://github.com/adswill/OnAir/blob/main/CONTRIBUTORS.md");
        ImGui::TextUnformatted("Licensed under the GNU General Public License v3.0 or later,");
        ImGui::TextUnformatted("with the additional attribution term in ADDITIONAL-TERMS.md.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void frame(App& a) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const bool running = a.engine.running();
    ImGui::Text("WW7ATS  Cougar Mountain ATV repeater   1255.000 MHz   6 MHz   DVB-T");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 60);
    if (ImGui::SmallButton("About")) a.showAbout = true;
    ImGui::Separator();

    // radio row
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(320);
    const char* cur = a.radio() ? a.radio()->name.c_str() : "(no radio found)";
    if (ImGui::BeginCombo("Radio", cur)) {
        for (int i = 0; i < (int)a.radios.size(); i++)
            if (ImGui::Selectable(a.radios[i].name.c_str(), i == a.radioIdx)) a.radioIdx = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) a.refreshRadios();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!running) { if (ImGui::Button("  Start  ")) a.start(); }
    else if (ImGui::Button("  Stop  ")) a.stop();

    // gain and volume
    const DeviceInfo* d = a.radio();
    ImGui::SetNextItemWidth(320);
    if (d && d->kind == DeviceInfo::HackRF) {
        if (ImGui::SliderInt("Gain", &a.hackrfGain, 0, ww7ats::kHackrfGainMax, "%d dB")) a.gainDirty = true;
        const ww7ats::HackrfGain g = ww7ats::splitHackrfGain(a.hackrfGain);
        ImGui::SameLine();
        if (ImGui::Checkbox("RF amp (+14 dB)", &a.amp)) a.gainDirty = true;
        ImGui::SameLine();
        ImGui::TextDisabled("LNA %d / VGA %d", g.lnaDb, g.vgaDb);
    } else {
        const float lo = d ? (float)d->gainMinDb : 0.f, hi = d && d->gainMaxDb > d->gainMinDb ? (float)d->gainMaxDb : 50.f;
        if (ImGui::SliderFloat("Gain", &a.genericGain, lo, hi, "%.0f dB")) a.gainDirty = true;
    }
    if (!running) a.gainDirty = false;
    ImGui::SetNextItemWidth(320);
    if (ImGui::SliderFloat("Volume", &a.volume, 0.f, 1.f, "%.2f")) a.engine.player().setVolume(a.volume);
    ImGui::SameLine();
    if (ImGui::Checkbox("Mute", &a.muted)) a.engine.player().setMuted(a.muted);

    // service picker, when the multiplex carries more than one
    if (running) {
        const TsSnapshot ts = a.engine.tsSnapshot();
        if (ts.services.size() > 1) {
            std::string label = "(none)";
            for (auto& sv : ts.services) if (sv.id == a.engine.player().selected()) label = sv.name.empty() ? std::to_string(sv.id) : sv.name;
            ImGui::SetNextItemWidth(320);
            if (ImGui::BeginCombo("Service", label.c_str())) {
                for (auto& sv : ts.services) {
                    const std::string n = sv.name.empty() ? "Service " + std::to_string(sv.id) : sv.name;
                    if (ImGui::Selectable(n.c_str(), sv.id == a.engine.player().selected()) && sv.havePmt) {
                        a.engine.player().select(sv.id); a.serviceId = sv.id; a.userPickedService = true; a.video.clear();
                    }
                }
                ImGui::EndCombo();
            }
        }
    }

    // status
    const auto& t = a.rx.dvbt;
    const bool playing = running && a.engine.player().selected() >= 0;
    const ww7ats::Lock lk = ww7ats::lockStage(running, a.rx.state, t.tpsOk, t.fecSync, playing);
    const ImVec4 lockCol = lk == ww7ats::Lock::Playing || lk == ww7ats::Lock::Fec ? ImVec4(0.4f, 0.9f, 0.5f, 1) : lk == ww7ats::Lock::Stopped ? ImVec4(0.6f, 0.6f, 0.6f, 1) : ImVec4(1, 0.75f, 0.3f, 1);
    ImGui::TextColored(lockCol, "%s", ww7ats::lockText(lk));
    if (running) {
        ImGui::SameLine();
        ImGui::Text("   SNR %.1f dB   CFO %+.1f kHz", a.rx.dataSnrDb, a.rx.cfoHz / 1e3);
        if (t.tpsOk) {
            ImGui::SameLine();
            ImGui::Text("   %s %s  GI %s  CR %s", ww7ats::fftName(t.mode), ww7ats::modName(t.mod), ww7ats::guardName(t.guard), ww7ats::rateName(t.crHp));
        }
        if (t.fecSync) {
            ImGui::SameLine();
            ImGui::Text("   RS fixed %llu, failed %llu", (unsigned long long)t.rsCorrected, (unsigned long long)t.rsFailed);
        }
    }
    if (playing) {
        const PlayerStats ps = a.engine.player().stats();
        ImGui::Separator();
        if (ps.hasAudio) {
            ImGui::Text("Audio: %s, %d ch", ps.audioCodec.c_str(), ps.audioChannels);
            ImGui::SameLine();
            if (ps.audioOutputActive) ImGui::TextDisabled("output active  %.0f ms buffered", ps.audioBufferMs);
            else ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", ps.audioOutputNote.empty() ? "sound output unavailable" : ps.audioOutputNote.c_str());
        } else {
            const auto tracks = a.engine.player().audioTracks();
            if (tracks.empty()) {
                ImGui::TextDisabled("Audio: waiting for an AAC/audio track");
            } else {
                ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Audio track detected, but its decoder did not start:");
                for (const AudioTrackInfo& track : tracks) {
                    ImGui::SameLine();
                    ImGui::Text("%s (%d ch)", track.codec.c_str(), track.channels);
                }
            }
            if (!ps.audioNote.empty()) ImGui::TextWrapped("%s", ps.audioNote.c_str());
        }
    }
    if (!a.message.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", a.message.c_str());
    else if (!a.radioErr.empty() && a.radios.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", a.radioErr.c_str());
    ImGui::Separator();

    // picture (left) and constellation (right)
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cs = std::max(160.f, std::min(avail.y - 24, avail.x * 0.38f));
    const float vw = std::max(100.f, avail.x - cs - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##video", ImVec2(vw, cs + 22), ImGuiChildFlags_None);
    ImGui::TextDisabled("Picture");
    if (a.video.w > 0) {
        const double ar = a.video.dar > 0 ? a.video.dar : (double)a.video.w / a.video.h;
        float w = vw, h = (float)(vw / ar);
        if (h > cs) { h = cs; w = (float)(cs * ar); }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (vw - w) * 0.5f);
        ImGui::Image((ImTextureID)(intptr_t)a.video.tex, ImVec2(w, h));
    } else {
        ImGui::Dummy(ImVec2(vw, cs * 0.4f));
        ImGui::TextDisabled(running ? "  no picture yet" : "  press Start");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Constellation (%zu cells)", a.rx.eqData.size());
    constellation(a, cs);
    ImGui::EndGroup();

    about(a);
    ImGui::End();
}

}  // namespace

int main(int, char**) {
    if (!cpuSupportsBuild()) { fprintf(stderr, "this build needs a processor with AVX2 and FMA\n"); return 1; }
    if (!glfwInit()) { fprintf(stderr, "GLFW failed to start\n"); return 1; }
#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    GLFWwindow* win = glfwCreateWindow(1100, 720, "WW7ATS receiver - DVB-T 1255 MHz", nullptr, nullptr);
    if (!win) { fprintf(stderr, "could not open a window (OpenGL 3 needed)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    {
        auto app = std::make_unique<App>();
        app->refreshRadios();
        while (!glfwWindowShouldClose(win)) {
            glfwPollEvents();
            app->poll();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            frame(*app);
            ImGui::Render();
            int fw, fh;
            glfwGetFramebufferSize(win, &fw, &fh);
            glViewport(0, 0, fw, fh);
            glClearColor(0.06f, 0.07f, 0.09f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(win);
        }
        if (app->engine.running()) app->stop();
    }   // the video texture goes while the GL context still exists

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
