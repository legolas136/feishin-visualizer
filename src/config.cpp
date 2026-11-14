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

void writeDefaults(const std::filesystem::path& path) {
    const Config c;
    std::ofstream out(path);
    out << "# feishin-visualizer settings. Restart the app after changes.\n"
        << "# Paths are relative to the folder of the .exe.\n"
        << "preset_dir=" << c.presetDir << "\n"
        << "texture_dir=" << c.textureDir << "\n"
        << "\n# Seconds a preset stays before a soft transition\n"
        << "preset_duration=" << c.presetDuration << "\n"
        << "# Seconds of blending between presets\n"
        << "soft_cut_duration=" << c.softCutDuration << "\n"
        << "\n# Hard cuts: instant switch on a strong beat, earliest after hard_cut_duration seconds\n"
        << "hard_cut_enabled=" << c.hardCutEnabled << "\n"
        << "hard_cut_duration=" << c.hardCutDuration << "\n"
        << "hard_cut_sensitivity=" << c.hardCutSensitivity << "\n"
        << "beat_sensitivity=" << c.beatSensitivity << "\n"
        << "\n# Warp mesh resolution (higher = smoother, slower)\n"
        << "mesh_width=" << c.meshWidth << "\n"
        << "mesh_height=" << c.meshHeight << "\n"
        << "fps=" << c.fps << "\n"
        << "vsync=" << c.vsync << "\n"
        << "aspect_correction=" << c.aspectCorrection << "\n"
        << "\nshuffle=" << c.shuffle << "\n"
        << "fullscreen=" << c.fullscreen << "\n"
        << "window_width=" << c.windowWidth << "\n"
        << "window_height=" << c.windowHeight << "\n";
}

}  // namespace

Config loadConfig(const std::filesystem::path& path) {
    Config c;

    std::ifstream in(path);
    if (!in) {
        writeDefaults(path);
        std::cout << "Created default config: " << path.u8string() << "\n";
        return c;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));

        try {
            if (key == "preset_dir") c.presetDir = value;
            else if (key == "texture_dir") c.textureDir = value;
            else if (key == "preset_duration") c.presetDuration = std::stod(value);
            else if (key == "soft_cut_duration") c.softCutDuration = std::stod(value);
            else if (key == "hard_cut_enabled") c.hardCutEnabled = toBool(value);
            else if (key == "hard_cut_duration") c.hardCutDuration = std::stod(value);
            else if (key == "hard_cut_sensitivity") c.hardCutSensitivity = std::stof(value);
            else if (key == "beat_sensitivity") c.beatSensitivity = std::stof(value);
            else if (key == "mesh_width") c.meshWidth = std::stoi(value);
            else if (key == "mesh_height") c.meshHeight = std::stoi(value);
            else if (key == "fps") c.fps = std::stoi(value);
            else if (key == "vsync") c.vsync = toBool(value);
            else if (key == "aspect_correction") c.aspectCorrection = toBool(value);
            else if (key == "shuffle") c.shuffle = toBool(value);
            else if (key == "fullscreen") c.fullscreen = toBool(value);
            else if (key == "window_width") c.windowWidth = std::stoi(value);
            else if (key == "window_height") c.windowHeight = std::stoi(value);
            else std::cerr << "Unknown config key: " << key << "\n";
        } catch (const std::exception&) {
            std::cerr << "Invalid value for " << key << ": " << value << "\n";
        }
    }

    return c;
}
