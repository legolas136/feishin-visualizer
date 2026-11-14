#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "beat_analysis.h"

struct ImGuiContext;

// Second window (F12): live view of the MilkDrop-style beat analysis and its settings.
class BeatWindow {
public:
    struct Status {
        std::string presetName;
        bool locked = false;
        double secondsToNextPreset = 0;
        std::string audioDevice;
        int sampleRate = 0;
    };

    struct Actions {
        bool save = false;
        bool beatSensitivityChanged = false;
    };

    ~BeatWindow();

    bool isOpen() const { return window_ != nullptr; }
    void open(SDL_Window* mainWindow, SDL_GLContext mainContext);
    void close();

    // Returns true if the event belonged to this window (and was consumed).
    bool handleEvent(const SDL_Event& e);

    // Records one frame of analysis for the graphs (call every frame, also when closed).
    void record(const BeatAnalysis& a, bool hardCut);

    Actions render(const BeatAnalysis& a, BeatSettings& s, float& beatSensitivity, const Status& status);

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    ImGuiContext* imgui_ = nullptr;

    static constexpr int kHistory = 600;
    struct Sample {
        float immRel[3];
        float avgRel[3];
        float thresh;
        bool cut;
    };
    std::vector<Sample> history_ = std::vector<Sample>(kHistory, Sample{{1, 1, 1}, {1, 1, 1}, 0, false});
    int pos_ = 0;
};
