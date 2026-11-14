#include <GL/glew.h>
#include <SDL.h>
#include <projectM-4/projectM.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "audio_capture.h"
#include "config.h"

namespace fs = std::filesystem;

namespace {

struct PresetList {
    std::vector<fs::path> files;
    std::vector<size_t> history;  // indices, last = current
    bool shuffle = true;
    std::mt19937 rng{std::random_device{}()};

    void scan(const fs::path& dir) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().extension() == ".milk") files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
    }

    size_t pickNext() {
        if (history.empty()) return shuffle ? randomIndex() : 0;
        if (shuffle) {
            size_t next = randomIndex();
            if (files.size() > 1) {
                while (next == history.back()) next = randomIndex();
            }
            return next;
        }
        return (history.back() + 1) % files.size();
    }

    size_t randomIndex() { return std::uniform_int_distribution<size_t>(0, files.size() - 1)(rng); }
};

struct App {
    projectm_handle pm = nullptr;
    SDL_Window* window = nullptr;
    PresetList presets;
    bool switchRequested = false;
    bool switchHardCut = false;
    int failedInARow = 0;
    bool locked = false;
};

void loadPreset(App& app, size_t index, bool hardCut) {
    app.presets.history.push_back(index);
    if (app.presets.history.size() > 200) app.presets.history.erase(app.presets.history.begin());

    const auto& file = app.presets.files[index];
    projectm_load_preset_file(app.pm, file.u8string().c_str(), !hardCut);

    std::string title = "feishin-visualizer - " + file.stem().u8string();
    if (app.locked) title += " [locked]";
    SDL_SetWindowTitle(app.window, title.c_str());
    std::cout << (hardCut ? "Hard cut: " : "Preset: ") << file.stem().u8string() << "\n";
}

void next(App& app, bool hardCut) {
    if (app.presets.files.empty()) return;
    loadPreset(app, app.presets.pickNext(), hardCut);
}

void previous(App& app) {
    auto& h = app.presets.history;
    if (h.size() < 2) return;
    h.pop_back();
    const size_t index = h.back();
    h.pop_back();
    loadPreset(app, index, false);
}

void onSwitchRequested(bool isHardCut, void* userData) {
    auto* app = static_cast<App*>(userData);
    app->switchRequested = true;
    app->switchHardCut = isHardCut;
}

void onSwitchFailed(const char* file, const char* message, void* userData) {
    auto* app = static_cast<App*>(userData);
    std::cerr << "Preset failed: " << file << " (" << message << ")\n";
    app->failedInARow++;
    app->switchRequested = true;
    app->switchHardCut = true;
}

void updateTitleLock(App& app) {
    projectm_set_preset_locked(app.pm, app.locked);
    if (!app.presets.history.empty()) {
        std::string title = "feishin-visualizer - " + app.presets.files[app.presets.history.back()].stem().u8string();
        if (app.locked) title += " [locked]";
        SDL_SetWindowTitle(app.window, title.c_str());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    fs::path baseDir = fs::current_path();
    if (char* base = SDL_GetBasePath()) {
        baseDir = fs::u8path(base);
        SDL_free(base);
    }

    const Config cfg = loadConfig(baseDir / "config.ini");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    App app;
    app.window = SDL_CreateWindow("feishin-visualizer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  cfg.windowWidth, cfg.windowHeight, flags);
    if (!app.window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(app.window);
    if (!gl) {
        std::cerr << "OpenGL 3.3 context failed: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "glewInit failed\n";
        return 1;
    }
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << " (" << glGetString(GL_RENDERER) << ")\n";

    app.pm = projectm_create();
    if (!app.pm) {
        std::cerr << "projectm_create failed\n";
        return 1;
    }

    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(app.window, &width, &height);
    projectm_set_window_size(app.pm, width, height);
    projectm_set_mesh_size(app.pm, cfg.meshWidth, cfg.meshHeight);
    projectm_set_fps(app.pm, cfg.fps);
    projectm_set_aspect_correction(app.pm, cfg.aspectCorrection);
    projectm_set_preset_duration(app.pm, cfg.presetDuration);
    projectm_set_soft_cut_duration(app.pm, cfg.softCutDuration);
    projectm_set_hard_cut_enabled(app.pm, cfg.hardCutEnabled);
    projectm_set_hard_cut_duration(app.pm, cfg.hardCutDuration);
    projectm_set_hard_cut_sensitivity(app.pm, cfg.hardCutSensitivity);
    projectm_set_beat_sensitivity(app.pm, cfg.beatSensitivity);

    const std::string texturePath = (baseDir / fs::u8path(cfg.textureDir)).u8string();
    const char* texturePaths[] = {texturePath.c_str()};
    projectm_set_texture_search_paths(app.pm, texturePaths, 1);

    projectm_set_preset_switch_requested_event_callback(app.pm, onSwitchRequested, &app);
    projectm_set_preset_switch_failed_event_callback(app.pm, onSwitchFailed, &app);

    app.presets.shuffle = cfg.shuffle;
    app.presets.scan(baseDir / fs::u8path(cfg.presetDir));
    std::cout << "Presets: " << app.presets.files.size() << " found\n";
    if (app.presets.files.empty()) {
        std::cerr << "No .milk presets found in " << (baseDir / fs::u8path(cfg.presetDir)).u8string() << "\n";
    }
    next(app, true);

    std::cout << "Keys: Right/N next, Left/P previous, H hard cut, L lock preset, F/F11 fullscreen, Esc quit\n";

    AudioCapture audio;
    std::vector<float> samples;
    const unsigned int maxFrames = projectm_pcm_get_max_samples();

    const Uint32 frameMs = (!cfg.vsync && cfg.fps > 0) ? 1000 / cfg.fps : 0;
    bool running = true;

    while (running) {
        const Uint32 frameStart = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                SDL_GL_GetDrawableSize(app.window, &width, &height);
                projectm_set_window_size(app.pm, width, height);
            } else if (e.type == SDL_KEYDOWN) {
                const bool isFullscreen = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        if (isFullscreen) SDL_SetWindowFullscreen(app.window, 0);
                        else running = false;
                        break;
                    case SDLK_f:
                    case SDLK_F11:
                        SDL_SetWindowFullscreen(app.window, isFullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                        break;
                    case SDLK_RIGHT:
                    case SDLK_n: next(app, false); break;
                    case SDLK_LEFT:
                    case SDLK_p: previous(app); break;
                    case SDLK_h: next(app, true); break;
                    case SDLK_l:
                        app.locked = !app.locked;
                        updateTitleLock(app);
                        break;
                    default: break;
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 2) {
                const bool isFullscreen = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(app.window, isFullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }

        if (app.switchRequested) {
            app.switchRequested = false;
            // Stop retrying if every preset fails, e.g. missing shader support.
            if (app.failedInARow < 20) next(app, app.switchHardCut);
        }

        audio.drain(samples);
        const size_t frames = samples.size() / 2;
        for (size_t offset = 0; offset < frames; offset += maxFrames) {
            const auto count = static_cast<unsigned int>(std::min<size_t>(maxFrames, frames - offset));
            projectm_pcm_add_float(app.pm, samples.data() + offset * 2, count, PROJECTM_STEREO);
        }

        glViewport(0, 0, width, height);
        projectm_opengl_render_frame(app.pm);
        SDL_GL_SwapWindow(app.window);

        if (!app.switchRequested && app.failedInARow > 0 && app.presets.history.size() > 0) {
            // A frame rendered without a failure callback: the current preset works.
            app.failedInARow = 0;
        }

        if (frameMs > 0) {
            const Uint32 elapsed = SDL_GetTicks() - frameStart;
            if (elapsed < frameMs) SDL_Delay(frameMs - elapsed);
        }
    }

    projectm_destroy(app.pm);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
