#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Compiles the shaders of the upcoming preset ahead of time: a second, invisible projectM
// instance on a background thread (own hidden window and GL context) loads it, which puts the
// linked shader programs into projectM's process-wide program binary cache (patched projectM).
// When the visible instance then switches to that preset, it loads the binaries instead of
// compiling, so Space/H and automatic switches do not stall the picture.
class PresetPrefetcher {
public:
    ~PresetPrefetcher();

    // Call on the main thread while the main GL context is current; restores it afterwards.
    bool start(SDL_Window* mainWindow, SDL_GLContext mainContext, const std::string& texturePath);
    void stop();

    // Warm up this preset next (replaces a preset that has not started yet).
    void prefetch(const std::string& presetText);

    int warmed() const { return warmed_; }
    // True if this exact preset text has finished warming up
    bool isWarm(const std::string& presetText) const { return warmedHash_ == std::hash<std::string>{}(presetText); }

private:
    void run();

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::string texturePath_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string pending_;
    bool hasPending_ = false;
    bool stop_ = false;
    std::atomic<int> warmed_{0};
    std::atomic<size_t> warmedHash_{0};
};
