#include "config.h"

#include <fstream>
#include <iostream>

namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r");
    return s.substr(begin, end - begin + 1);
}

bool toBool(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace

void saveConfig(const std::filesystem::path& path, const Config& c) {
    std::ofstream out(path);
    const BeatSettings& b = c.beat;
    out << "# feishin-visualizer settings. Edit here or in the beat window (F12).\n"
        << "# Paths are relative to the folder of the .exe.\n"
        << "preset_dir=" << c.presetDir << "\n"
        << "texture_dir=" << c.textureDir << "\n"
        << "\n# Preset switching (MilkDrop 2 defaults: 16 / 10 / 2.7 / 1.7)\n"
        << "# Seconds until the next automatic preset, plus a random extra of up to ..._rand\n"
        << "time_between_presets=" << b.timeBetweenPresets << "\n"
        << "time_between_presets_rand=" << b.timeBetweenPresetsRand << "\n"
        << "# Blend seconds for automatic switches and for manual ones (Space, menu)\n"
        << "blend_time_auto=" << b.blendTimeAuto << "\n"
        << "blend_time_user=" << b.blendTimeUser << "\n"
        << "\n# Hard cuts: instant switch on loud beats (MilkDrop defaults: off / 2.5 / 60)\n"
        << "hard_cuts_enabled=" << b.hardCutsEnabled << "\n"
        << "hard_cut_loudness_thresh=" << b.hardCutLoudnessThresh << "\n"
        << "hard_cut_halflife=" << b.hardCutHalflife << "\n"
        << "# 0 = analyse the left channel only (like MilkDrop), 1 = mix left and right\n"
        << "analysis_stereo_mix=" << b.stereoMix << "\n"
        << "# Samples per analysis: 576 = MilkDrop (fast, jumpy), 1024 / 2048 = calmer, better bass\n"
        << "analysis_window=" << b.windowSamples << "\n"
        << "\n# How strongly presets react to the music (projectM)\n"
        << "beat_sensitivity=" << c.beatSensitivity << "\n"
        << "\n# Sync: low_latency = render without queued frames, audio_delay_ms = delay visuals\n"
        << "vsync=" << c.vsync << "\n"
        << "low_latency=" << c.lowLatency << "\n"
        << "audio_delay_ms=" << c.audioDelayMs << "\n"
        << "\n# Warp mesh resolution (higher = smoother, slower). Restart after changes below.\n"
        << "mesh_width=" << c.meshWidth << "\n"
        << "mesh_height=" << c.meshHeight << "\n"
        << "fps=" << c.fps << "\n"
        << "aspect_correction=" << c.aspectCorrection << "\n"
        << "\nfullscreen=" << c.fullscreen << "\n"
        << "window_width=" << c.windowWidth << "\n"
        << "window_height=" << c.windowHeight << "\n";
}

Config loadConfig(const std::filesystem::path& path) {
    Config c;
    BeatSettings& b = c.beat;

    std::ifstream in(path);
    std::string line;
    while (in && std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));

        try {
            if (key == "preset_dir") c.presetDir = value;
            else if (key == "texture_dir") c.textureDir = value;
            else if (key == "time_between_presets") b.timeBetweenPresets = std::stof(value);
            else if (key == "time_between_presets_rand") b.timeBetweenPresetsRand = std::stof(value);
            else if (key == "blend_time_auto") b.blendTimeAuto = std::stof(value);
            else if (key == "blend_time_user") b.blendTimeUser = std::stof(value);
            else if (key == "hard_cuts_enabled") b.hardCutsEnabled = toBool(value);
            else if (key == "hard_cut_loudness_thresh") b.hardCutLoudnessThresh = std::stof(value);
            else if (key == "hard_cut_halflife") b.hardCutHalflife = std::stof(value);
            else if (key == "analysis_stereo_mix") b.stereoMix = toBool(value);
            else if (key == "analysis_window") b.windowSamples = std::stoi(value);
            else if (key == "beat_sensitivity") c.beatSensitivity = std::stof(value);
            else if (key == "mesh_width") c.meshWidth = std::stoi(value);
            else if (key == "mesh_height") c.meshHeight = std::stoi(value);
            else if (key == "fps") c.fps = std::stoi(value);
            else if (key == "vsync") c.vsync = toBool(value);
            else if (key == "low_latency") c.lowLatency = toBool(value);
            else if (key == "audio_delay_ms") c.audioDelayMs = std::stof(value);
            else if (key == "aspect_correction") c.aspectCorrection = toBool(value);
            else if (key == "fullscreen") c.fullscreen = toBool(value);
            else if (key == "window_width") c.windowWidth = std::stoi(value);
            else if (key == "window_height") c.windowHeight = std::stoi(value);
            else std::cerr << "Ignoring old/unknown config key: " << key << "\n";
        } catch (const std::exception&) {
            std::cerr << "Invalid value for " << key << ": " << value << "\n";
        }
    }

    // Same limits as MilkDrop's MyReadConfig
    if (b.timeBetweenPresetsRand < 0) b.timeBetweenPresetsRand = 0;
    if (b.timeBetweenPresets < 0.1f) b.timeBetweenPresets = 0.1f;
    if (b.windowSamples != 1024 && b.windowSamples != 2048) b.windowSamples = 576;

    saveConfig(path, c);
    return c;
}
