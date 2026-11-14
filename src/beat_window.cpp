#include "beat_window.h"

#include <GL/glew.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

const ImU32 kBandColors[3] = {IM_COL32(255, 90, 90, 255), IM_COL32(90, 220, 90, 255), IM_COL32(90, 150, 255, 255)};
const char* kBandNames[3] = {"bass", "mid", "treb"};

}  // namespace

BeatWindow::~BeatWindow() { close(); }

void BeatWindow::open(SDL_Window* mainWindow, SDL_GLContext mainContext) {
    if (window_) return;
    window_ = SDL_CreateWindow("feishin-visualizer - beat detection", 900, 760,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) return;
    gl_ = SDL_GL_CreateContext(window_);
    SDL_GL_SetSwapInterval(0);  // never block the visualizer's frame on this window

    ImGuiContext* previous = ImGui::GetCurrentContext();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.3f);
    ImGui::GetStyle().FontScaleMain = 1.3f;
    ImGui_ImplSDL3_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init("#version 330");
    ImGui::SetCurrentContext(previous);

    SDL_GL_MakeCurrent(mainWindow, mainContext);
}

void BeatWindow::close() {
    if (!window_) return;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    SDL_GLContext prevGl = SDL_GL_GetCurrentContext();
    SDL_Window* prevWin = SDL_GL_GetCurrentWindow();

    SDL_GL_MakeCurrent(window_, gl_);
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(imgui_);
    ImGui::SetCurrentContext(previous == imgui_ ? nullptr : previous);
    SDL_GL_DestroyContext(gl_);
    SDL_DestroyWindow(window_);
    if (prevWin && prevWin != window_) SDL_GL_MakeCurrent(prevWin, prevGl);
    window_ = nullptr;
    gl_ = nullptr;
    imgui_ = nullptr;
}

bool BeatWindow::handleEvent(const SDL_Event& e) {
    if (!window_) return false;
    if (SDL_GetWindowFromEvent(&e) != window_) return false;

    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        close();
        return true;
    }
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplSDL3_ProcessEvent(&e);
    ImGui::SetCurrentContext(previous);
    return true;
}

void BeatWindow::record(const BeatAnalysis& a, bool hardCut) {
    Sample& s = history_[pos_];
    for (int i = 0; i < 3; i++) {
        s.immRel[i] = a.immRel[i];
        s.avgRel[i] = a.avgRel[i];
    }
    s.thresh = a.hardCutThresh;
    s.cut = hardCut;
    pos_ = (pos_ + 1) % kHistory;
}

BeatWindow::Actions BeatWindow::render(const BeatAnalysis& a, Config& cfg, const Status& status) {
    Actions actions;
    if (!window_) return actions;
    BeatSettings& s = cfg.beat;
    float& beatSensitivity = cfg.beatSensitivity;

    // Capture age: average and maximum per second
    const double t = SDL_GetTicks() / 1000.0;
    ageSum_ += status.captureAgeMs;
    ageMax_ = std::max(ageMax_, status.captureAgeMs);
    ageCount_++;
    if (t - ageWindowStart_ >= 1.0) {
        ageAvgShown_ = ageSum_ / std::max(1, ageCount_);
        ageMaxShown_ = ageMax_;
        ageSum_ = ageMax_ = 0;
        ageCount_ = 0;
        ageWindowStart_ = t;
    }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    SDL_GLContext prevGl = SDL_GL_GetCurrentContext();
    SDL_Window* prevWin = SDL_GL_GetCurrentWindow();

    SDL_GL_MakeCurrent(window_, gl_);
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("beat", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    // --- Status
    ImGui::Text("Preset: %s%s", status.presetName.c_str(), status.locked ? "  [locked]" : "");
    if (status.locked) ImGui::Text("Next preset: locked (Scroll Lock)");
    else ImGui::Text("Next preset in %.1f s", std::max(0.0, status.secondsToNextPreset));
    ImGui::Text("Audio: %s, %d Hz", status.audioDevice.empty() ? "no device" : status.audioDevice.c_str(),
                status.sampleRate);

    // --- Levels as MilkDrop presets see them
    ImGui::SeparatorText("Levels (1.0 = average of the song)");
    for (int i = 0; i < 3; i++) {
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%s %.2f   %s_att %.2f", kBandNames[i], a.immRel[i], kBandNames[i],
                      a.avgRel[i]);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kBandColors[i]);
        ImGui::ProgressBar(std::min(a.immRel[i] / 3.0f, 1.0f), ImVec2(-1, 0), overlay);
        ImGui::PopStyleColor();
    }

    // --- History graph: bass/mid/treb, their sum/3 and the hard cut threshold
    ImGui::SeparatorText("Last 10 s: bass / mid / treb, white = (bass+mid+treb)/3, yellow = hard cut threshold");
    {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 200);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(20, 20, 24, 255));
        const float maxV = 4.0f;
        auto yOf = [&](float v) { return p0.y + size.y - std::clamp(v / maxV, 0.0f, 1.0f) * size.y; };
        for (int g = 1; g < 4; g++) {
            dl->AddLine(ImVec2(p0.x, yOf(static_cast<float>(g))), ImVec2(p0.x + size.x, yOf(static_cast<float>(g))),
                        IM_COL32(60, 60, 70, 255));
        }
        const float dx = size.x / (kHistory - 1);
        for (int k = 1; k < kHistory; k++) {
            const Sample& prev = history_[(pos_ + k - 1) % kHistory];
            const Sample& cur = history_[(pos_ + k) % kHistory];
            const float x0 = p0.x + (k - 1) * dx, x1 = p0.x + k * dx;
            if (cur.cut) dl->AddLine(ImVec2(x1, p0.y), ImVec2(x1, p0.y + size.y), IM_COL32(255, 60, 255, 255), 2.0f);
            for (int i = 0; i < 3; i++) {
                dl->AddLine(ImVec2(x0, yOf(prev.immRel[i])), ImVec2(x1, yOf(cur.immRel[i])), kBandColors[i]);
            }
            const float sumPrev = (prev.immRel[0] + prev.immRel[1] + prev.immRel[2]) / 3.0f;
            const float sumCur = (cur.immRel[0] + cur.immRel[1] + cur.immRel[2]) / 3.0f;
            dl->AddLine(ImVec2(x0, yOf(sumPrev)), ImVec2(x1, yOf(sumCur)), IM_COL32(255, 255, 255, 255), 1.5f);
            if (s.hardCutsEnabled) {
                dl->AddLine(ImVec2(x0, yOf(prev.thresh)), ImVec2(x1, yOf(cur.thresh)), IM_COL32(255, 220, 60, 255), 2.0f);
            }
        }
        ImGui::Dummy(size);
        ImGui::TextDisabled("Grid lines at 1, 2, 3. Magenta lines = hard cuts.");
    }

    // --- Spectrum with MilkDrop's band ranges
    ImGui::SeparatorText("Spectrum (MilkDrop FFT, equalized) and band ranges");
    {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 110);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(20, 20, 24, 255));
        const int bins = a.bandEdges[3];
        float peak = 1e-6f;
        for (int i = 0; i < bins; i++) peak = std::max(peak, a.spectrum[i]);
        const float bw = size.x / std::max(1, bins);
        for (int i = 0; i < bins; i++) {
            int band = i < a.bandEdges[1] ? 0 : (i < a.bandEdges[2] ? 1 : 2);
            const float h = std::sqrt(a.spectrum[i] / peak) * size.y;
            dl->AddRectFilled(ImVec2(p0.x + i * bw, p0.y + size.y - h), ImVec2(p0.x + (i + 1) * bw, p0.y + size.y),
                              kBandColors[band]);
        }
        ImGui::Dummy(size);
    }

    // --- Settings
    ImGui::SeparatorText("Preset switching");
    ImGui::SliderFloat("time between presets (s)", &s.timeBetweenPresets, 0.1f, 120.0f, "%.1f");
    ImGui::SliderFloat("random extra time (s)", &s.timeBetweenPresetsRand, 0.0f, 60.0f, "%.1f");
    ImGui::SliderFloat("blend time, automatic (s)", &s.blendTimeAuto, 0.0f, 10.0f, "%.1f");
    ImGui::SliderFloat("blend time, manual (s)", &s.blendTimeUser, 0.0f, 10.0f, "%.1f");

    ImGui::SeparatorText("Hard cuts");
    ImGui::Checkbox("hard cuts enabled", &s.hardCutsEnabled);
    ImGui::SliderFloat("loudness threshold", &s.hardCutLoudnessThresh, 1.0f, 5.0f, "%.2f");
    ImGui::SetItemTooltip("A hard cut fires when bass+mid+treb > 3 x threshold.\n"
                          "After a cut the threshold doubles, then decays back to this value.");
    ImGui::SliderFloat("threshold half-life (s)", &s.hardCutHalflife, 1.0f, 120.0f, "%.0f");

    ImGui::SeparatorText("Analysis");
    ImGui::Checkbox("mix left + right (MilkDrop: left only)", &s.stereoMix);
    if (ImGui::SliderFloat("preset reaction strength (projectM)", &beatSensitivity, 0.0f, 5.0f, "%.2f")) {
        actions.beatSensitivityChanged = true;
    }
    ImGui::SetItemTooltip("projectM still computes the bass/mid/treb values inside the presets itself.\n"
                          "This scales how strongly presets react.");

    ImGui::SeparatorText("Latency / sync");
    const float frames = cfg.lowLatency ? 1.0f : (cfg.vsync ? 2.5f : 1.5f);
    ImGui::Text("Audio capture -> analysis: %.0f ms avg, %.0f ms max", ageAvgShown_, ageMaxShown_);
    ImGui::Text("Frame time: %.1f ms   estimated render -> screen: ~%.0f ms (+ monitor)", status.frameMs,
                status.frameMs * frames);
    ImGui::Text("Delay setting: %.0f ms   => visuals lag the captured audio by ~%.0f ms",
                cfg.audioDelayMs, ageAvgShown_ + status.frameMs * frames + cfg.audioDelayMs);
    ImGui::TextDisabled("Captured audio is what Windows sends to the device. Devices with their own lag\n"
                        "(wireless/Bluetooth, large interface buffers) play it later: raise the delay for them.");
    if (ImGui::Checkbox("VSync", &cfg.vsync)) actions.vsyncChanged = true;
    ImGui::SameLine();
    ImGui::Checkbox("low latency (no queued frames)", &cfg.lowLatency);
    ImGui::SliderFloat("audio delay (ms)", &cfg.audioDelayMs, 0.0f, 500.0f, "%.0f");

    ImGui::Spacing();
    if (ImGui::Button("Save to config.ini")) actions.save = true;
    ImGui::SameLine();
    if (ImGui::Button("MilkDrop defaults")) {
        s = BeatSettings{};
        beatSensitivity = 1.0f;
        actions.beatSensitivityChanged = true;
    }

    ImGui::End();
    ImGui::Render();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);

    ImGui::SetCurrentContext(previous);
    SDL_GL_MakeCurrent(prevWin, prevGl);
    return actions;
}
