#pragma once

#include <filesystem>
#include <string>

struct Config {
    std::string presetDir = "presets";
    std::string textureDir = "textures";

    double presetDuration = 30.0;
    double softCutDuration = 3.0;
    bool hardCutEnabled = true;
    double hardCutDuration = 20.0;
    float hardCutSensitivity = 2.0f;
    float beatSensitivity = 1.0f;

    int meshWidth = 48;
    int meshHeight = 36;
    int fps = 60;
    bool vsync = true;
    bool aspectCorrection = true;

    bool shuffle = true;
    bool fullscreen = false;
    int windowWidth = 1280;
    int windowHeight = 720;
};

// Reads key=value pairs from `path`. Writes a file with the defaults if it does not exist.
Config loadConfig(const std::filesystem::path& path);
