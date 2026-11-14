#include <GL/glew.h>
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <projectM-4/projectM.h>

#include <algorithm>
#include <cfloat>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "audio_capture.h"
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

double now() { return SDL_GetTicks64() / 1000.0; }

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
    projectm_load_preset_data(app.pm, preset.text.c_str(), !hardCut);
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
    LoadedPreset p{file, file.stem().u8string(), readTextFile(file)};
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
    if (const auto file = app.library->pickNext(app.current.file)) loadPresetFile(app, *file, hardCut);
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

void toggleFullscreen(App& app) {
    const bool isFullscreen = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(app.window, isFullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

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

void handleMenuKey(App& app, const SDL_Keysym& key) {
    LoadMenu& m = app.menu;
    const int page = 15;
    switch (key.sym) {
        case SDLK_ESCAPE:
        case SDLK_l: m.open = false; break;
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
            } else if (key.sym != SDLK_RIGHT) {
                // Loading from a folder makes it the pool for random/sequential switching.
                if (app.library->directory() != m.dir) {
                    app.library->setDirectory(m.dir);
                    saveState(app);
                }
                loadPresetFile(app, entry.path, (key.mod & KMOD_SHIFT) != 0);
            }
            break;
        }
        default: break;
    }
}

void handlePathKey(App& app, const SDL_Keysym& key) {
    switch (key.sym) {
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
        case SDLK_v:
            if (key.mod & KMOD_CTRL) {
                if (char* clip = SDL_GetClipboardText()) {
                    app.pathText += clip;
                    SDL_free(clip);
                }
            }
            break;
        default: break;
    }
}

void handleKey(App& app, const SDL_Keysym& key, bool& running) {
    const bool alt = key.mod & KMOD_ALT;
    if (alt && (key.sym == SDLK_RETURN || key.sym == SDLK_KP_ENTER)) {
        toggleFullscreen(app);
        return;
    }
    if (app.pathInput) return handlePathKey(app, key);
    if (app.menu.open) return handleMenuKey(app, key);

    switch (key.sym) {
        case SDLK_ESCAPE:
            if (app.showHelp) app.showHelp = false;
            else if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) toggleFullscreen(app);
            else running = false;
            break;
        case SDLK_F11: toggleFullscreen(app); break;

        // Preset navigation
        case SDLK_SPACE: next(app, false); break;
        case SDLK_h: next(app, true); break;
        case SDLK_BACKSPACE: back(app); break;
        case SDLK_r:
            app.library->random = !app.library->random;
            saveState(app);
            notify(app, app.library->random ? "random preset order" : "sequential preset order");
            break;
        case SDLK_l:
            app.menu.show(app.library->directory(), app.current.file);
            app.swallowText = true;
            break;
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
        case SDLK_EQUALS: changeRating(app, 1); break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: changeRating(app, -1); break;
        case SDLK_a: aggregate(app); break;
        case SDLK_d:
            app.shaderLock = (app.shaderLock + 1) % 4;
            notify(app, shaderLockName(app.shaderLock));
            break;

        // Overlays
        case SDLK_F1: app.showHelp = !app.showHelp; break;
        case SDLK_F2:
        case SDLK_F3:
        case SDLK_t: notify(app, "song info needs the Feishin link (coming)"); break;
        case SDLK_F4: app.showPresetName = !app.showPresetName; break;
        case SDLK_F5: app.showFps = !app.showFps; break;
        case SDLK_F6: app.showRating = !app.showRating; break;
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
    "F4        preset name    F5 fps    F6 rating",
    "F8        type a preset directory (Ctrl+V pastes)",
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
    ImGui_ImplSDL2_InitForOpenGL(app.window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");

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

    PresetLibrary library(baseDir / fs::u8path(cfg.presetDir), baseDir / "ratings.ini");
    app.library = &library;
    app.stateFile = baseDir / "state.ini";
    loadState(app);
    std::cout << "Presets: " << library.size() << " in " << library.directory().u8string() << "\n";
    if (library.size() == 0) notify(app, "No .milk presets found - press L or F8");
    next(app, true);

    std::cout << "Press F1 for keys\n";

    AudioCapture audio;
    std::vector<float> samples;
    const unsigned int maxFrames = projectm_pcm_get_max_samples();

    const Uint32 frameMs = (!cfg.vsync && cfg.fps > 0) ? 1000 / cfg.fps : 0;
    bool running = true;
    float fps = 0;
    Uint64 lastFrame = SDL_GetPerformanceCounter();

    while (running) {
        const Uint32 frameStart = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                SDL_GL_GetDrawableSize(app.window, &width, &height);
                projectm_set_window_size(app.pm, width, height);
            } else if (e.type == SDL_KEYDOWN) {
                handleKey(app, e.key.keysym, running);
            } else if (e.type == SDL_TEXTINPUT) {
                if (app.swallowText) app.swallowText = false;
                else if (app.pathInput) app.pathText += e.text.text;
                else if (app.menu.open) app.menu.jumpTo(e.text.text, now());
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 2) {
                toggleFullscreen(app);
            }
        }
        app.swallowText = false;

        // Scroll Lock LED = preset lock, as in MilkDrop.
        const bool scrollLock = (SDL_GetModState() & KMOD_SCROLL) != 0;
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
        const size_t frames = samples.size() / 2;
        for (size_t offset = 0; offset < frames; offset += maxFrames) {
            const auto count = static_cast<unsigned int>(std::min<size_t>(maxFrames, frames - offset));
            projectm_pcm_add_float(app.pm, samples.data() + offset * 2, count, PROJECTM_STEREO);
        }

        glViewport(0, 0, width, height);
        projectm_opengl_render_frame(app.pm);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawOverlay(app, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y), fps);
        ImGui::Render();
        glViewport(0, 0, width, height);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(app.window);

        if (!app.switchRequested && app.failedInARow > 0) app.failedInARow = 0;

        const Uint64 t = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(t - lastFrame) / static_cast<float>(SDL_GetPerformanceFrequency());
        lastFrame = t;
        if (dt > 0) fps = fps * 0.95f + (1.0f / dt) * 0.05f;

        if (frameMs > 0) {
            const Uint32 elapsed = SDL_GetTicks() - frameStart;
            if (elapsed < frameMs) SDL_Delay(frameMs - elapsed);
        }
    }

    saveState(app);
    projectm_destroy(app.pm);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
