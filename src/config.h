#pragma once

#include <filesystem>
#include <string>

#include "beat_analysis.h"
#include "beat_tracker.h"

struct Config {
    std::string presetDir = "presets";
    std::string textureDir = "textures";

    // Preset switching, blending and hard cuts (MilkDrop 2 settings and defaults)
    BeatSettings beat;
    TrackerSettings tracker;
    // Presets react to our analysis (window, stereo mix) instead of projectM's own (patched projectM)
    bool presetsUseAnalysis = true;
    // Scales how far bass/mid/treb move away from 1.0 for the presets (1 = MilkDrop)
    float reactionStrength = 1.0f;

    int meshWidth = 48;
    int meshHeight = 36;
    int fps = 60;
    bool vsync = true;
    bool lowLatency = false;    // glFinish after each frame: no frames queued in the driver
    float audioDelayMs = 0.0f;  // delay audio into the analysis/visuals (for outputs with lag, e.g. wireless)
    bool aspectCorrection = true;

    bool fullscreen = false;
    int windowWidth = 1280;
    int windowHeight = 720;
};

// Reads key=value pairs from `path`. Missing keys keep their defaults; the file is
// (re)written afterwards so it always lists every setting.
Config loadConfig(const std::filesystem::path& path);
void saveConfig(const std::filesystem::path& path, const Config& c);
