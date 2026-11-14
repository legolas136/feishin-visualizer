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
    // Tracked beat -> presets: pulses mixed into bass (or all bands), see beat window "Beat tracker"
    float beatInjectMix = 0.5f;     // 0 = off, 1 = presets see only the beat pulse
    float beatInjectHeight = 1.5f;  // pulse peak above the song average (1.0)
    float beatInjectDecay = 0.15f;  // seconds
    bool beatInjectAllBands = false;
    // Switch presets every N tracked beats instead of by time (0 = off)
    int switchEveryBeats = 0;
    bool switchOnBeatHardCut = false;
    bool switchOnDownbeat = true;  // beat-synced switches wait for the "1" when it is known

    int meshWidth = 48;
    int meshHeight = 36;
    int maxFps = 0;  // 0 = follow the monitor refresh rate
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
