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

BeatWindow::Actions BeatWindow::render(const BeatAnalysis& a, const BeatTracker& tracker, Config& cfg,
                                       const Status& status) {
    Actions actions;
    if (!window_) return actions;
    BeatSettings& s = cfg.beat;

    // Capture age: average and maximum per second
    const double t = SDL_GetTicks() / 1000.0;
    ageSum_ += status.captureAgeMs;
    ageMax_ = std::max(ageMax_, status.captureAgeMs);
    ageCount_++;
    if (t - ageWindowStart_ >= 1.0) {
        ageAvgShown_ = ageSum_ / std::max(1, ageCount_);
        ageMaxShown_ = ageMax_;
        ageSum_ = 0;
        ageMax_ = -1e9f;
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
    ImGui::Text("Audio: %s, analysed at %d Hz", status.audioDevice.empty() ? "no device" : status.audioDevice.c_str(),
                status.sampleRate);

    // --- Levels as MilkDrop presets see them
    if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem("MilkDrop analysis")) {
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
    ImGui::SeparatorText("Last 10 s: bass / mid / treb, white = average of the three, yellow = hard cut threshold");
    ImGui::Checkbox("show smoothed values (bass_att / mid_att / treb_att, what most presets use)", &showAtt_);
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
            const float* pv = showAtt_ ? prev.avgRel : prev.immRel;
            const float* cv = showAtt_ ? cur.avgRel : cur.immRel;
            for (int i = 0; i < 3; i++) {
                dl->AddLine(ImVec2(x0, yOf(pv[i])), ImVec2(x1, yOf(cv[i])), kBandColors[i]);
            }
            const float sumPrev = (pv[0] + pv[1] + pv[2]) / 3.0f;
            const float sumCur = (cv[0] + cv[1] + cv[2]) / 3.0f;
            dl->AddLine(ImVec2(x0, yOf(sumPrev)), ImVec2(x1, yOf(sumCur)), IM_COL32(255, 255, 255, 255), 1.5f);
            if (s.hardCutsEnabled && !showAtt_) {
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
    {
        const int sizes[3] = {576, 1024, 2048};
        const char* labels[3] = {"576 samples (MilkDrop, fast and jumpy)", "1024 samples (calmer)",
                                 "2048 samples (calm, best bass)"};
        int current = s.windowSamples == 2048 ? 2 : (s.windowSamples == 1024 ? 1 : 0);
        if (ImGui::Combo("analysis window", &current, labels, 3)) s.windowSamples = sizes[current];
        ImGui::SetItemTooltip("MilkDrop analyses the newest 576 samples (~13 ms) per frame. Longer windows\n"
                              "react a little later but are steadier and measure low bass better.");
    }
    ImGui::Checkbox("presets react to this analysis", &cfg.presetsUseAnalysis);
    ImGui::SetItemTooltip("On: presets get bass/mid/treb from the analysis shown here (window, stereo mix,\n"
                          "reaction strength apply). Off: projectM's own analysis (same as MilkDrop, 576 / left).");
    ImGui::BeginDisabled(!cfg.presetsUseAnalysis);
    ImGui::SliderFloat("reaction strength", &cfg.reactionStrength, 0.0f, 3.0f, "%.2f");
    ImGui::SetItemTooltip("How far bass/mid/treb move away from 1.0 for the presets.\n"
                          "1 = MilkDrop, below 1 = calmer, above 1 = wilder.");
    ImGui::EndDisabled();

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

        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Beat tracker")) {
        drawTracker(tracker, cfg.tracker, status);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    }

    ImGui::Spacing();
    if (ImGui::Button("Save to config.ini")) actions.save = true;
    ImGui::SameLine();
    if (ImGui::Button("Defaults (MilkDrop)")) {
        s = BeatSettings{};
        cfg.tracker = TrackerSettings{};
        cfg.reactionStrength = 1.0f;
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

void BeatWindow::drawTracker(const BeatTracker& t, TrackerSettings& s, const Status& status) {
    const float fr = t.frameRate();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // --- Lamp + state
    const char* stateName = "searching";
    ImU32 lampColor = IM_COL32(150, 150, 150, 255);
    if (t.state == TrackState::Locked) {
        stateName = "locked";
        lampColor = IM_COL32(80, 255, 120, 255);
    } else if (t.state == TrackState::Coasting) {
        stateName = "coasting (silence, keeps the beat)";
        lampColor = IM_COL32(255, 190, 60, 255);
    }
    const float radius = 46.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 c(p0.x + radius + 4, p0.y + radius + 4);
    const float flash = t.state == TrackState::Searching ? 0.0f : std::exp(-static_cast<float>(status.lastBeatAgo) / 0.15f);
    dl->AddCircleFilled(c, radius, IM_COL32(35, 35, 40, 255), 48);
    const ImU32 lit = (lampColor & 0x00FFFFFF) | (static_cast<ImU32>(40 + 215 * flash) << 24);
    dl->AddCircleFilled(c, radius * (0.75f + 0.25f * flash), lit, 48);
    dl->AddCircle(c, radius, lampColor, 48, 2.0f);

    // onset lamp (raw detection, no tracking)
    long lastOnset = -1;
    for (long f = t.frame() - 1; f >= 0 && f >= t.frame() - BeatTracker::kHistory; f--) {
        if (t.flags[f % BeatTracker::kHistory] & BeatTracker::FlagOnset) {
            lastOnset = f;
            break;
        }
    }
    const float onsetAgo = lastOnset < 0 ? 1e9f : (t.frame() - lastOnset) / fr;
    const ImVec2 oc(c.x + radius + 30, c.y + radius - 12);
    dl->AddCircleFilled(oc, 12, IM_COL32(60, 220, 255, static_cast<int>(40 + 215 * std::exp(-onsetAgo / 0.1f))), 24);

    // beat counter (1-4, no downbeat detection)
    for (int k = 0; k < 4; k++) {
        const bool on = t.state != TrackState::Searching && (t.beatCount - 1) % 4 == k;
        dl->AddCircleFilled(ImVec2(c.x - radius + 6 + k * 26.0f, c.y + radius + 20), 8,
                            on ? lampColor : IM_COL32(60, 60, 70, 255), 16);
    }

    ImGui::SetCursorScreenPos(ImVec2(c.x + radius + 60, p0.y));
    ImGui::BeginGroup();
    ImGui::Text("State: %s", stateName);
    if (t.state == TrackState::Searching) ImGui::Text("BPM: -");
    else ImGui::Text("BPM: %.1f", t.bpm());
    ImGui::Text("Tempo estimate: %.1f BPM (confidence %.2f, lock at %.2f)", t.estimateBpm, t.estimateConfidence,
                s.acquireConfidence);
    ImGui::ProgressBar(t.confidence, ImVec2(260, 0), "beat match confidence");
    ImGui::SameLine();
    ImGui::ProgressBar(t.beatPhase(), ImVec2(160, 0), "phase");
    ImGui::Text("Signal: %s   tempo differs for %.1f s   phase differs for %.1f s", t.silent ? "silent" : "audio",
                t.mismatchSeconds, t.phaseMismatchSeconds);
    ImGui::TextDisabled("Big lamp = tracked beat, small cyan lamp = raw onset, dots = beat 1-4 (not bar-aligned)");
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 2 * radius + 40));

    // --- Onset function history
    ImGui::SeparatorText("Last 6 s: onset strength (white), threshold (yellow), bass onsets (red)");
    {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 180);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(q, ImVec2(q.x + size.x, q.y + size.y), IM_COL32(20, 20, 24, 255));
        const int frames = std::min<long>(static_cast<int>(6.0f * fr), BeatTracker::kHistory - 1);
        const long end = t.frame();
        const long start = std::max<long>(0, end - frames);
        float peak = 1.0f;
        for (long f = start; f < end; f++) peak = std::max(peak, t.odf[f % BeatTracker::kHistory]);
        const float dx = size.x / frames;
        auto yOf = [&](float v) { return q.y + size.y - std::clamp(v / peak, 0.0f, 1.0f) * (size.y - 16); };
        for (long f = start + 1; f < end; f++) {
            const int i0 = static_cast<int>((f - 1) % BeatTracker::kHistory);
            const int i1 = static_cast<int>(f % BeatTracker::kHistory);
            const float x0 = q.x + (f - 1 - (end - frames)) * dx, x1 = x0 + dx;
            const uint8_t fl = t.flags[i1];
            if (fl & BeatTracker::FlagSilent) {
                dl->AddRectFilled(ImVec2(x0, q.y), ImVec2(x1, q.y + size.y), IM_COL32(30, 30, 60, 255));
            }
            if (fl & BeatTracker::FlagBeat) {
                dl->AddLine(ImVec2(x1, q.y), ImVec2(x1, q.y + size.y), IM_COL32(80, 255, 120, 160), 2.0f);
            }
            if (fl & BeatTracker::FlagCorrected) {
                dl->AddTriangleFilled(ImVec2(x1 - 5, q.y), ImVec2(x1 + 5, q.y), ImVec2(x1, q.y + 9),
                                      IM_COL32(255, 80, 255, 255));
            }
            if (fl & BeatTracker::FlagOnset) {
                const ImU32 col = (fl & BeatTracker::FlagMatched) ? IM_COL32(80, 255, 120, 255) : IM_COL32(60, 220, 255, 255);
                dl->AddRectFilled(ImVec2(x1 - 2, q.y + size.y - 10), ImVec2(x1 + 2, q.y + size.y), col);
            }
            dl->AddLine(ImVec2(x0, yOf(t.odfPhase[i0])), ImVec2(x1, yOf(t.odfPhase[i1])), IM_COL32(255, 90, 90, 140));
            dl->AddLine(ImVec2(x0, yOf(t.threshold[i0])), ImVec2(x1, yOf(t.threshold[i1])), IM_COL32(255, 220, 60, 200));
            dl->AddLine(ImVec2(x0, yOf(t.odf[i0])), ImVec2(x1, yOf(t.odf[i1])), IM_COL32(255, 255, 255, 255), 1.5f);
        }
        ImGui::Dummy(size);
        ImGui::TextDisabled("Green lines = tracked beats, magenta = corrected beat position, blue/cyan ticks = onsets "
                            "(green = matched a beat), dark blue = silence");
    }

    // --- Tempo curve
    ImGui::SeparatorText("Tempo candidates (autocorrelation x tempo preference)");
    if (!t.tempoCurve.empty()) {
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 90);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(q, ImVec2(q.x + size.x, q.y + size.y), IM_COL32(20, 20, 24, 255));
        const int nb = static_cast<int>(t.tempoCurve.size());
        float peak = 1e-6f;
        for (float v : t.tempoCurve) peak = std::max(peak, v);
        const float bw = size.x / nb;
        for (int b = 0; b < nb; b++) {
            const float h = t.tempoCurve[b] / peak * (size.y - 14);
            dl->AddRectFilled(ImVec2(q.x + b * bw, q.y + size.y - h), ImVec2(q.x + (b + 1) * bw - 1, q.y + size.y),
                              IM_COL32(110, 110, 140, 255));
        }
        auto xOf = [&](float bpmValue) { return q.x + (bpmValue - t.tempoCurveMinBpm + 0.5f) * bw; };
        dl->AddLine(ImVec2(xOf(t.estimateBpm), q.y), ImVec2(xOf(t.estimateBpm), q.y + size.y), IM_COL32(255, 255, 255, 255), 2.0f);
        if (t.state != TrackState::Searching) {
            dl->AddLine(ImVec2(xOf(t.bpm()), q.y), ImVec2(xOf(t.bpm()), q.y + size.y), IM_COL32(80, 255, 120, 255), 2.0f);
        }
        for (int bv = static_cast<int>(std::ceil(t.tempoCurveMinBpm / 20.0f) * 20); bv < t.tempoCurveMinBpm + nb; bv += 20) {
            char label[8];
            std::snprintf(label, sizeof(label), "%d", bv);
            dl->AddText(ImVec2(xOf(static_cast<float>(bv)), q.y), IM_COL32(150, 150, 160, 255), label);
        }
        ImGui::Dummy(size);
        ImGui::TextDisabled("White = estimate, green = tracked tempo");
    }

    // --- Settings
    ImGui::SeparatorText("Tracker settings");
    ImGui::DragFloatRange2("tempo range (BPM)", &s.minBpm, &s.maxBpm, 1.0f, 40.0f, 240.0f, "%.0f", "%.0f",
                           ImGuiSliderFlags_AlwaysClamp);
    if (s.maxBpm < s.minBpm + 20) s.maxBpm = s.minBpm + 20;
    ImGui::SliderFloat("preferred tempo (BPM)", &s.preferredBpm, s.minBpm, s.maxBpm, "%.0f");
    ImGui::SetItemTooltip("Centre of the tempo preference, decides between half and double tempo.");
    ImGui::SliderFloat("onset sensitivity", &s.sensitivity, 0.3f, 3.0f, "%.2f");
    ImGui::SliderFloat("bass weight", &s.bassWeight, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("0 = all frequencies, 1 = kick drum range (30-180 Hz) only.");
    ImGui::SliderFloat("phase correction", &s.phaseGain, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("How strongly a matching onset pulls the beat. Low = steady lock, high = follows quickly.");
    ImGui::SliderFloat("tempo correction", &s.tempoGain, 0.0f, 0.2f, "%.3f");
    ImGui::SliderFloat("lock threshold", &s.acquireConfidence, 0.05f, 0.8f, "%.2f");
    ImGui::SetItemTooltip("Tempo confidence needed to lock. Higher = fewer false locks on beatless music.");
    ImGui::SliderFloat("survive silence (s)", &s.coastSeconds, 0.0f, 60.0f, "%.0f");
    ImGui::SliderFloat("re-lock after (s)", &s.relockSeconds, 1.0f, 15.0f, "%.1f");
    ImGui::SetItemTooltip("A different tempo (new track, tempo change) must persist this long before re-locking.");
    ImGui::Checkbox("automatic preset switches wait for the next beat", &s.alignSwitches);
}
