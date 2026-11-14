#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <projectM-4/projectM.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "audio_capture.h"
#include "beat_analysis.h"
#include "beat_tracker.h"
#include "beat_window.h"
#include "config.h"
#include "load_menu.h"
#include "milk_text.h"
#include "preset_library.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kHistorySize = 64;  // like MilkDrop 2

struct LoadedPreset {
    fs::path file;  // file the base of the preset came from (ratings refer to it)
    std::string name;
    std::string text;
};

enum ShaderLock { LockNone = 0, LockWarp = 1, LockComp = 2, LockBoth = 3 };

struct App {
    projectm_handle pm = nullptr;
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    Config cfg;
    fs::path configFile;
    BeatAnalysis analysis;
    BeatTracker tracker;
    double lastBeatWall = -1e9;
    double switchDueSince = -1;  // automatic switch waiting for a beat
    int beatsSincePreset = 0;
    float injectMix = 0;  // beat pulse mix, faded in/out with the tracker lock
    float refreshHz = 60;  // monitor refresh rate of the window's display
    int projectmFps = 0;   // value last passed to projectm_set_fps
    // Preset switch timing (ms): pick, file read, projectM load, first frame after the load
    double pickMs = 0, readMs = 0, loadMs = 0, firstFrameMs = 0;
    int framesSinceLoad = 1000;
    std::string switchTiming;
    BeatWindow beat;
    bool autoSwitch = false;  // next load is an automatic switch (uses blend_time_auto)
    PresetLibrary* library = nullptr;
    fs::path stateFile;

    std::deque<LoadedPreset> history;  // back() = current
    LoadedPreset current;

    int shaderLock = LockNone;
    bool locked = false;

    bool switchRequested = false;
    bool switchHardCut = false;
    int failedInARow = 0;

    // Overlays (F-keys)
    bool showHelp = false;
    bool showPresetName = true;
    bool showFps = false;
    bool showRating = false;

    LoadMenu menu;
    bool pathInput = false;  // F8
    std::string pathText;

    std::string notice;
    double noticeUntil = 0;

    ImFont* font = nullptr;
    float fontSize = 22.0f;
    bool swallowText = false;  // the "l" that opened the menu also arrives as text
};

double now() { return SDL_GetTicks() / 1000.0; }

void notify(App& app, const std::string& text) {
    app.notice = text;
    app.noticeUntil = now() + 2.5;
    std::cout << text << "\n";
}

const char* shaderLockName(int lock) {
    switch (lock) {
        case LockWarp: return "warp shader locked";
        case LockComp: return "composite shader locked";
        case LockBoth: return "warp + composite shaders locked";
        default: return "shaders unlocked";
    }
}

void saveState(const App& app) {
    std::ofstream out(app.stateFile);
    out << "random=" << app.library->random << "\n";
    out << "directory=" << app.library->directory().u8string() << "\n";
}

void loadState(App& app) {
    std::ifstream in(app.stateFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "random") app.library->random = value == "1";
        if (key == "directory") {
            const auto dir = fs::u8path(value);
            std::error_code ec;
            if (fs::is_directory(dir, ec)) app.library->setDirectory(dir);
        }
    }
}

void updateTitle(App& app) {
    std::string title = "feishin-visualizer - " + app.current.name;
    if (app.locked) title += " [locked]";
    SDL_SetWindowTitle(app.window, title.c_str());
}

void applyPreset(App& app, LoadedPreset preset, bool hardCut, bool pushHistory) {
    const BeatSettings& b = app.cfg.beat;
    projectm_set_soft_cut_duration(app.pm, app.autoSwitch ? b.blendTimeAuto : b.blendTimeUser);
    app.autoSwitch = false;
    app.beatsSincePreset = 0;
    app.analysis.restartTimer(now(), b);
    const Uint64 t0 = SDL_GetTicksNS();
    projectm_load_preset_data(app.pm, preset.text.c_str(), !hardCut);
    app.loadMs = (SDL_GetTicksNS() - t0) / 1e6;
    app.framesSinceLoad = 0;
    app.current = std::move(preset);
    if (pushHistory) {
        app.history.push_back(app.current);
        while (app.history.size() > kHistorySize) app.history.pop_front();
    }
    updateTitle(app);
    std::cout << (hardCut ? "Hard cut: " : "Preset: ") << app.current.name << "\n";
}

// Loads a preset file, keeping locked shaders (D) from the current preset.
void loadPresetFile(App& app, const fs::path& file, bool hardCut) {
    const Uint64 t0 = SDL_GetTicksNS();
    LoadedPreset p{file, file.stem().u8string(), readTextFile(file)};
    app.readMs = (SDL_GetTicksNS() - t0) / 1e6;
    if (p.text.empty()) {
        notify(app, "Cannot read " + file.u8string());
        return;
    }
    if (app.shaderLock != LockNone && !app.current.text.empty()) {
        MilkParts parts = splitMilk(p.text);
        const MilkParts cur = splitMilk(app.current.text);
        if (app.shaderLock & LockWarp) parts.warp = cur.warp;
        if (app.shaderLock & LockComp) parts.comp = cur.comp;
        p.text = joinMilk(parts);
        p.name += " (" + std::string(shaderLockName(app.shaderLock)) + ")";
    }
    applyPreset(app, std::move(p), hardCut, true);
}

void next(App& app, bool hardCut) {
    const Uint64 t0 = SDL_GetTicksNS();
    const auto file = app.library->pickNext(app.current.file);
    app.pickMs = (SDL_GetTicksNS() - t0) / 1e6;
    if (file) loadPresetFile(app, *file, hardCut);
}

void updateRefreshRate(App& app) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(app.window));
    app.refreshHz = (mode && mode->refresh_rate > 0) ? mode->refresh_rate : 60.0f;
    std::cout << "Display: " << app.refreshHz << " Hz\n";
}

void back(App& app) {
    if (app.history.size() < 2) return;
    app.history.pop_back();
    applyPreset(app, app.history.back(), false, false);
}

// "A": random preset + warp shader of a second + composite shader of a third.
void aggregate(App& app) {
    const auto a = app.library->pickRandom();
    const auto b = app.library->pickRandom();
    const auto c = app.library->pickRandom();
    if (!a || !b || !c) return;

    MilkParts parts = splitMilk(readTextFile(*a));
    if (!(app.shaderLock & LockWarp)) parts.warp = splitMilk(readTextFile(*b)).warp;
    else parts.warp = splitMilk(app.current.text).warp;
    if (!(app.shaderLock & LockComp)) parts.comp = splitMilk(readTextFile(*c)).comp;
    else parts.comp = splitMilk(app.current.text).comp;

    std::string name = "mash-up: " + a->stem().u8string();
    if (!(app.shaderLock & LockWarp)) name += " | warp: " + b->stem().u8string();
    if (!(app.shaderLock & LockComp)) name += " | comp: " + c->stem().u8string();
    applyPreset(app, {*a, name, joinMilk(parts)}, false, true);
}

void changeRating(App& app, int delta) {
    if (app.current.file.empty()) return;
    const int r = std::clamp(app.library->rating(app.current.file) + delta, 0, 5);
    app.library->setRating(app.current.file, r);
    notify(app, "rating: " + std::to_string(r) + "/5");
}

bool isFullscreen(App& app) { return (SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN) != 0; }

void toggleFullscreen(App& app) { SDL_SetWindowFullscreen(app.window, !isFullscreen(app)); }

void onSwitchRequested(bool isHardCut, void* userData) {
    auto* app = static_cast<App*>(userData);
    app->switchRequested = true;
    app->switchHardCut = isHardCut;
}

void onSwitchFailed(const char* file, const char* message, void* userData) {
    auto* app = static_cast<App*>(userData);
    std::cerr << "Preset failed: " << app->current.name << " " << (file ? file : "") << " ("
              << (message ? message : "") << ")\n";
    app->failedInARow++;
    app->switchRequested = true;
    app->switchHardCut = true;
}

// ---------------------------------------------------------------------------
// Key handling

void handleMenuKey(App& app, const SDL_KeyboardEvent& key) {
    LoadMenu& m = app.menu;
    const int page = 15;
    switch (key.key) {
        case SDLK_ESCAPE:
        case SDLK_L: m.open = false; break;
        case SDLK_UP: m.move(-1); break;
        case SDLK_DOWN: m.move(1); break;
        case SDLK_PAGEUP: m.move(-page); break;
        case SDLK_PAGEDOWN: m.move(page); break;
        case SDLK_HOME: m.move(-1000000); break;
        case SDLK_END: m.move(1000000); break;
        case SDLK_LEFT:
        case SDLK_BACKSPACE:
            if (m.dir.has_parent_path() && m.dir.parent_path() != m.dir) m.show(m.dir.parent_path(), m.dir);
            break;
        case SDLK_RIGHT:
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            if (m.entries.empty()) break;
            const auto entry = m.entries[m.selected];
            if (entry.isDir) {
                m.show(entry.path, entry.label == ".." ? m.dir : fs::path{});
            } else if (key.key != SDLK_RIGHT) {
                // Loading from a folder makes it the pool for random/sequential switching.
                if (app.library->directory() != m.dir) {
                    app.library->setDirectory(m.dir);
                    saveState(app);
                }
                loadPresetFile(app, entry.path, (key.mod & SDL_KMOD_SHIFT) != 0);
            }
            break;
        }
        default: break;
    }
}

void handlePathKey(App& app, const SDL_KeyboardEvent& key) {
    switch (key.key) {
        case SDLK_ESCAPE: app.pathInput = false; break;
        case SDLK_BACKSPACE:
            if (!app.pathText.empty()) app.pathText.pop_back();
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            app.pathInput = false;
            const auto dir = fs::u8path(app.pathText);
            std::error_code ec;
            if (fs::is_directory(dir, ec)) app.menu.show(dir);
            else notify(app, "Not a directory: " + app.pathText);
            break;
        }
        case SDLK_V:
            if (key.mod & SDL_KMOD_CTRL) {
                if (char* clip = SDL_GetClipboardText()) {
                    app.pathText += clip;
                    SDL_free(clip);
                }
            }
            break;
        default: break;
    }
}

void handleKey(App& app, const SDL_KeyboardEvent& key, bool& running) {
    const bool alt = key.mod & SDL_KMOD_ALT;
    if (alt && (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER)) {
        toggleFullscreen(app);
        return;
    }
    if (app.pathInput) return handlePathKey(app, key);
    if (app.menu.open) return handleMenuKey(app, key);

    switch (key.key) {
        case SDLK_ESCAPE:
            if (app.showHelp) app.showHelp = false;
            else if (isFullscreen(app)) toggleFullscreen(app);
            else running = false;
            break;
        case SDLK_F11: toggleFullscreen(app); break;

        // Preset navigation
        case SDLK_SPACE: next(app, false); break;
        case SDLK_H: next(app, true); break;
        case SDLK_BACKSPACE: back(app); break;
        case SDLK_R:
            app.library->random = !app.library->random;
            saveState(app);
            notify(app, app.library->random ? "random preset order" : "sequential preset order");
            break;
        case SDLK_L:
            app.menu.show(app.library->directory(), app.current.file);
            app.swallowText = true;
            break;
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
        case SDLK_EQUALS: changeRating(app, 1); break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: changeRating(app, -1); break;
        case SDLK_A: aggregate(app); break;
        case SDLK_D:
            app.shaderLock = (app.shaderLock + 1) % 4;
            notify(app, shaderLockName(app.shaderLock));
            break;

        // Overlays
        case SDLK_1:
            if (app.tracker.state == TrackState::Searching) {
                notify(app, "no beat locked yet");
            } else {
                app.tracker.markDownbeat();
                notify(app, "bar start set: this is the 1");
            }
            break;
        case SDLK_F1: app.showHelp = !app.showHelp; break;
        case SDLK_F2:
        case SDLK_F3:
        case SDLK_T: notify(app, "song info needs the Feishin link (coming)"); break;
        case SDLK_F4: app.showPresetName = !app.showPresetName; break;
        case SDLK_F5: app.showFps = !app.showFps; break;
        case SDLK_F6: app.showRating = !app.showRating; break;
        case SDLK_F12:
            if (app.beat.isOpen()) app.beat.close();
            else app.beat.open(app.window, app.gl);
            break;
        case SDLK_F8:
            app.pathInput = true;
            app.pathText = app.library->directory().u8string();
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Overlay drawing

void drawText(App& app, float x, float y, const std::string& text, ImU32 color, bool alignRight = false,
              float size = 0) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (size <= 0) size = app.fontSize;
    if (alignRight) x -= app.font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
    dl->AddText(app.font, size, ImVec2(x + 2, y + 2), IM_COL32(0, 0, 0, 200), text.c_str());
    dl->AddText(app.font, size, ImVec2(x, y), color, text.c_str());
}

const char* kHelp[] = {
    "F1        this help",
    "SPACE     next preset (blend)",
    "H         next preset (hard cut)",
    "BACKSPACE previous preset",
    "R         random / sequential order",
    "L         load menu (arrows, PgUp/PgDn, type to jump,",
    "          Enter load, Shift+Enter hard cut, Left = up)",
    "SCROLL LOCK  lock current preset (LED on = locked)",
    "+ / -     rate current preset (0 = never random)",
    "A         mash-up: preset + warp + comp of others",
    "D         cycle warp/composite shader lock",
    "1         this beat is the 1 (bar start for the beat tracker)",
    "F4        preset name    F5 fps    F6 rating",
    "F8        type a preset directory (Ctrl+V pastes)",
    "F12       beat detection window (graphs + settings)",
    "ALT+ENTER / F11 / double click   fullscreen",
    "ESC       close menu, leave fullscreen, quit",
    "",
    "Coming with the Feishin link: F2 F3 T, Z X C V B, arrows",
};

void drawOverlay(App& app, int width, int height, float fps) {
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    const ImU32 yellow = IM_COL32(255, 220, 90, 255);
    const ImU32 grey = IM_COL32(190, 190, 190, 255);
    const float line = app.fontSize + 4;
    const float pad = 12;

    // Top right column: preset name, rating, fps, notice
    float y = pad;
    const float right = width - pad;
    if (app.showPresetName && !app.current.name.empty()) {
        drawText(app, right, y, app.current.name, white, true);
        y += line;
    }
    if (app.showRating && !app.current.file.empty()) {
        drawText(app, right, y, "rating: " + std::to_string(app.library->rating(app.current.file)) + "/5", white, true);
        y += line;
    }
    if (app.showFps) {
        drawText(app, right, y, std::to_string(static_cast<int>(fps + 0.5f)) + " fps", white, true);
        y += line;
    }
    if (app.locked) {
        drawText(app, right, y, "preset locked", yellow, true);
        y += line;
    }
    if (now() < app.noticeUntil) drawText(app, right, y, app.notice, yellow, true);

    // Top left: help, load menu or path prompt
    if (app.pathInput) {
        drawText(app, pad, pad, "Preset directory (Enter = open, Esc = cancel):", yellow);
        drawText(app, pad, pad + line, app.pathText + "_", white);
    } else if (app.menu.open) {
        const LoadMenu& m = app.menu;
        drawText(app, pad, pad, "Load preset: " + m.dir.u8string(), yellow);
        const int visible = std::max(1, static_cast<int>((height - 3 * line) / line));
        const int first = std::clamp(m.selected - visible / 2, 0, std::max(0, static_cast<int>(m.entries.size()) - visible));
        for (int i = first; i < static_cast<int>(m.entries.size()) && i < first + visible; i++) {
            const auto& e = m.entries[i];
            std::string label = e.label;
            if (!e.isDir) label += "  (" + std::to_string(app.library->rating(e.path)) + ")";
            if (i == m.selected) label = "> " + label;
            else label = "  " + label;
            drawText(app, pad, pad + line * (1 + i - first), label,
                     i == m.selected ? yellow : (e.isDir ? grey : white));
        }
    } else if (app.showHelp) {
        float hy = pad;
        for (const char* l : kHelp) {
            drawText(app, pad, hy, l, white, false, app.fontSize * 0.85f);
            hy += line * 0.85f;
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    fs::path baseDir = fs::current_path();
    if (const char* base = SDL_GetBasePath()) baseDir = fs::u8path(base);

    const Config cfg = loadConfig(baseDir / "config.ini");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    App app;
    app.window = SDL_CreateWindow("feishin-visualizer", cfg.windowWidth, cfg.windowHeight, flags);
    if (!app.window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(app.window);
    app.gl = gl;
    app.cfg = cfg;
    app.configFile = baseDir / "config.ini";
    if (!gl) {
        std::cerr << "OpenGL 3.3 context failed: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);
    updateRefreshRate(app);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "glewInit failed\n";
        return 1;
    }
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << " (" << glGetString(GL_RENDERER) << ")\n";

    // ImGui for text overlays
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const float fontSize = app.fontSize;
    std::error_code ec;
    for (const char* f : {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        if (fs::exists(f, ec)) {
            app.font = io.Fonts->AddFontFromFileTTF(f, fontSize);
            if (app.font) break;
        }
    }
    if (!app.font) {
        ImFontConfig fc;
        fc.SizePixels = fontSize;
        app.font = io.Fonts->AddFontDefault(&fc);
    }
    ImGui_ImplSDL3_InitForOpenGL(app.window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    app.pm = projectm_create();
    if (!app.pm) {
        std::cerr << "projectm_create failed\n";
        return 1;
    }

    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app.window, &width, &height);
    projectm_set_window_size(app.pm, width, height);
    projectm_set_mesh_size(app.pm, cfg.meshWidth, cfg.meshHeight);
    projectm_set_aspect_correction(app.pm, cfg.aspectCorrection);
    // Switching and hard cuts are driven by our MilkDrop-style analysis, not by projectM.
    projectm_set_preset_duration(app.pm, 1e7);
    projectm_set_hard_cut_enabled(app.pm, false);
    projectm_set_soft_cut_duration(app.pm, cfg.beat.blendTimeUser);

    const std::string texturePath = (baseDir / fs::u8path(cfg.textureDir)).u8string();
    const char* texturePaths[] = {texturePath.c_str()};
    projectm_set_texture_search_paths(app.pm, texturePaths, 1);

    projectm_set_preset_switch_requested_event_callback(app.pm, onSwitchRequested, &app);
    projectm_set_preset_switch_failed_event_callback(app.pm, onSwitchFailed, &app);

    PresetLibrary library(baseDir / fs::u8path(cfg.presetDir), baseDir / "ratings.ini");
    app.library = &library;
    app.stateFile = baseDir / "state.ini";
    loadState(app);
    std::cout << "Presets: " << library.size() << " in " << library.directory().u8string() << "\n";
    if (library.size() == 0) notify(app, "No presets found: unpack the preset pack next to the .exe (see README) or press F8");
    next(app, true);

    std::cout << "Press F1 for keys\n";

    AudioCapture audio;
    std::vector<float> samples;
    std::deque<float> delayLine;  // audio_delay_ms
    float frameMs = 0;
    const unsigned int maxFrames = projectm_pcm_get_max_samples();

    SDL_StartTextInput(app.window);
    bool running = true;
    float fps = 0;
    Uint64 lastFrame = SDL_GetPerformanceCounter();

    while (running) {
        const Uint64 frameStart = SDL_GetTicksNS();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (app.beat.handleEvent(e)) continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                updateRefreshRate(app);
            } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                SDL_GetWindowSizeInPixels(app.window, &width, &height);
                projectm_set_window_size(app.pm, width, height);
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                handleKey(app, e.key, running);
            } else if (e.type == SDL_EVENT_TEXT_INPUT) {
                if (app.swallowText) app.swallowText = false;
                else if (app.pathInput) app.pathText += e.text.text;
                else if (app.menu.open) app.menu.jumpTo(e.text.text, now());
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.clicks == 2) {
                toggleFullscreen(app);
            }
        }
        app.swallowText = false;

        // Scroll Lock LED = preset lock, as in MilkDrop.
        const bool scrollLock = (SDL_GetModState() & SDL_KMOD_SCROLL) != 0;
        if (scrollLock != app.locked) {
            app.locked = scrollLock;
            projectm_set_preset_locked(app.pm, app.locked);
            updateTitle(app);
            notify(app, app.locked ? "preset locked" : "preset unlocked");
        }

        if (app.switchRequested) {
            app.switchRequested = false;
            // Stop retrying if every preset fails, e.g. missing shader support.
            if (app.failedInARow < 20 && !app.locked) next(app, app.switchHardCut);
        }

        audio.drain(samples);
        if (app.cfg.audioDelayMs > 0.5f || !delayLine.empty()) {
            // Hold audio back so visuals line up with outputs that play late.
            const size_t hold = 2 * static_cast<size_t>(app.cfg.audioDelayMs * audio.sampleRate() / 1000.0f);
            delayLine.insert(delayLine.end(), samples.begin(), samples.end());
            samples.clear();
            if (delayLine.size() > hold) {
                const size_t release = (delayLine.size() - hold) & ~static_cast<size_t>(1);
                samples.assign(delayLine.begin(), delayLine.begin() + release);
                delayLine.erase(delayLine.begin(), delayLine.begin() + release);
            }
        }
        const size_t frames = samples.size() / 2;
        for (size_t offset = 0; offset < frames; offset += maxFrames) {
            const auto count = static_cast<unsigned int>(std::min<size_t>(maxFrames, frames - offset));
            projectm_pcm_add_float(app.pm, samples.data() + offset * 2, count, PROJECTM_STEREO);
        }

        // MilkDrop-style analysis: hard cuts and timed preset switches
        app.analysis.setSampleRate(audio.sampleRate());
        app.analysis.addSamples(samples.data(), frames, app.cfg.beat.stereoMix);
        app.tracker.setSampleRate(audio.sampleRate());
        const int trackedBeats = app.tracker.process(samples.data(), frames, app.cfg.tracker);
        if (trackedBeats > 0) app.lastBeatWall = now();
        app.beatsSincePreset += trackedBeats;
        const bool beatLocked = app.tracker.state != TrackState::Searching;
        const bool hardCut = app.analysis.update(fps, now(), app.locked, app.cfg.beat);
        app.beat.record(app.analysis, hardCut);
        if (app.cfg.presetsUseAnalysis) {
            // Presets react to our analysis (patched projectM), scaled around 1.0
            const float k = app.cfg.reactionStrength;

            // Tracked beat as a pulse, held at the full mix while the beat is on. Off at once on real
            // silence (back within ~150 ms) or when the rhythm suddenly stops; fades with the track
            // when it fades out (level below -8 dB of its loudest part).
            const float fade = app.tracker.loudnessFade;
            float target = (app.tracker.silent || !app.tracker.beatActive) ? 0.0f : app.cfg.beatInjectMix * fade;
            if (app.cfg.pulseOverride) target = app.cfg.beatInjectMix;  // test switch
            if (target < app.injectMix) app.injectMix = target;
            else app.injectMix += (target - app.injectMix) * std::min(1.0f, frameMs / 150.0f);
            const float sinceBeat = static_cast<float>(now() - app.lastBeatWall);
            const float decay = std::max(0.01f, app.cfg.beatInjectDecay);
            const float pulse = 1.0f + app.cfg.beatInjectHeight * std::exp(-sinceBeat / decay);
            const float pulseAtt = 1.0f + 0.6f * app.cfg.beatInjectHeight * std::exp(-sinceBeat / (4.0f * decay));

            float levels[6];
            for (int i = 0; i < 3; i++) {
                float imm = app.analysis.immRel[i];
                float att = app.analysis.avgRel[i];
                if (i == 0 || app.cfg.beatInjectAllBands) {
                    imm = (1.0f - app.injectMix) * imm + app.injectMix * pulse;
                    att = (1.0f - app.injectMix) * att + app.injectMix * pulseAtt;
                }
                levels[i] = std::max(0.0f, 1.0f + (imm - 1.0f) * k);
                levels[3 + i] = std::max(0.0f, 1.0f + (att - 1.0f) * k);
            }
            projectm_set_audio_levels_override(app.pm, levels);
        } else {
            projectm_set_audio_levels_override(app.pm, nullptr);
        }
        const bool beatSwitching = app.cfg.switchEveryBeats > 0 && beatLocked;
        if (hardCut) {
            next(app, true);
        } else if (beatSwitching) {
            const int bar = app.tracker.barPosition();
            const bool onTheOne = !app.cfg.switchOnDownbeat || bar < 0 || bar == 0;
            if (!app.locked && trackedBeats > 0 && app.beatsSincePreset >= app.cfg.switchEveryBeats && onTheOne) {
                app.autoSwitch = true;
                next(app, app.cfg.switchOnBeatHardCut);
            }
        } else if (!app.locked && app.analysis.presetTimeUp(now())) {
            // Optionally wait for the next tracked beat (at most 2 s)
            if (app.switchDueSince < 0) app.switchDueSince = now();
            const bool waitForBeat = app.cfg.tracker.alignSwitches && app.tracker.state != TrackState::Searching;
            if (!waitForBeat || trackedBeats > 0 || now() - app.switchDueSince > 2.0) {
                app.switchDueSince = -1;
                app.autoSwitch = true;
                next(app, false);
            }
        }

        glViewport(0, 0, width, height);
        // projectM's frame rate (used for time-based preset effects) follows the real target
        const int targetFps = app.cfg.maxFps > 0 ? app.cfg.maxFps : static_cast<int>(app.refreshHz + 0.5f);
        if (targetFps != app.projectmFps) {
            app.projectmFps = targetFps;
            projectm_set_fps(app.pm, targetFps);
        }
        const Uint64 renderStart = SDL_GetTicksNS();
        projectm_opengl_render_frame(app.pm);
        if (app.framesSinceLoad == 0) {
            // Shader compilation of a new preset often happens in its first frame
            app.firstFrameMs = (SDL_GetTicksNS() - renderStart) / 1e6;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "pick %.1f ms, read %.1f ms, projectM load %.1f ms, first frame %.1f ms",
                          app.pickMs, app.readMs, app.loadMs, app.firstFrameMs);
            app.switchTiming = buf;
            std::cout << "  switch: " << buf << "\n";
        }
        app.framesSinceLoad++;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawOverlay(app, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y), fps);
        ImGui::Render();
        glViewport(0, 0, width, height);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(app.window);
        if (app.cfg.lowLatency) glFinish();  // wait for the GPU: no frames queued ahead

        if (app.beat.isOpen()) {
            BeatWindow::Status st{app.current.name, app.locked, app.analysis.nextPresetTime() - now(),
                                  audio.deviceName(), audio.sampleRate(), audio.captureAgeMs(), frameMs,
                                  now() - app.lastBeatWall,
                                  (app.cfg.switchEveryBeats > 0 && app.tracker.state != TrackState::Searching)
                                      ? std::max(0, app.cfg.switchEveryBeats - app.beatsSincePreset)
                                      : -1,
                                  app.refreshHz, app.switchTiming, app.injectMix};
            const auto act = app.beat.render(app.analysis, app.tracker, app.cfg, st);
            if (act.vsyncChanged) SDL_GL_SetSwapInterval(app.cfg.vsync ? 1 : 0);
            if (act.save) {
                saveConfig(app.configFile, app.cfg);
                notify(app, "settings saved to config.ini");
            }
        }

        if (!app.switchRequested && app.failedInARow > 0) app.failedInARow = 0;

        const Uint64 t = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(t - lastFrame) / static_cast<float>(SDL_GetPerformanceFrequency());
        lastFrame = t;
        if (dt > 0) {
            fps = fps * 0.95f + (1.0f / dt) * 0.05f;
            frameMs = frameMs * 0.95f + dt * 1000.0f * 0.05f;
        }

        // Frame limit (only needed without VSync or below the monitor rate)
        if (app.cfg.maxFps > 0 && (!app.cfg.vsync || app.cfg.maxFps < app.refreshHz - 1.0f)) {
            const Uint64 frameNs = 1000000000ull / static_cast<Uint64>(app.cfg.maxFps);
            const Uint64 elapsed = SDL_GetTicksNS() - frameStart;
            if (elapsed < frameNs) SDL_DelayPrecise(frameNs - elapsed);
        }
    }

    saveState(app);
    app.beat.close();
    projectm_destroy(app.pm);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
