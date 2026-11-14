#include <GL/glew.h>  // before any other GL header

#include "preset_prefetcher.h"

#include <projectM-4/projectM.h>

#include <iostream>

PresetPrefetcher::~PresetPrefetcher() { stop(); }

bool PresetPrefetcher::start(SDL_Window* mainWindow, SDL_GLContext mainContext, const std::string& texturePath) {
    texturePath_ = texturePath;
    window_ = SDL_CreateWindow("feishin-visualizer prefetch", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window_) return false;
    context_ = SDL_GL_CreateContext(window_);  // becomes current here
    SDL_GL_MakeCurrent(mainWindow, mainContext);
    if (!context_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void PresetPrefetcher::stop() {
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    if (context_) {
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void PresetPrefetcher::prefetch(const std::string& presetText) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = presetText;
        hasPending_ = true;
    }
    cv_.notify_all();
}

void PresetPrefetcher::run() {
    if (!SDL_GL_MakeCurrent(window_, context_)) {
        std::cerr << "Prefetch: cannot use GL context: " << SDL_GetError() << "\n";
        return;
    }
    projectm_handle pm = projectm_create();
    if (!pm) {
        std::cerr << "Prefetch: projectm_create failed\n";
        SDL_GL_MakeCurrent(window_, nullptr);
        return;
    }
    projectm_set_window_size(pm, 64, 64);
    const char* paths[] = {texturePath_.c_str()};
    projectm_set_texture_search_paths(pm, paths, 1);
    projectm_set_preset_duration(pm, 1e7);
    projectm_set_hard_cut_enabled(pm, false);

    for (;;) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || hasPending_; });
            if (stop_) break;
            text.swap(pending_);
            hasPending_ = false;
        }
        // Loading compiles the preset's shaders (into the shared program cache); one frame
        // compiles whatever is created lazily on first use.
        projectm_load_preset_data(pm, text.c_str(), false);
        projectm_opengl_render_frame(pm);
        glFinish();
        warmedHash_ = std::hash<std::string>{}(text);
        warmed_++;
    }

    projectm_destroy(pm);
    SDL_GL_MakeCurrent(window_, nullptr);
}
