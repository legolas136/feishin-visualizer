#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Directory browser for the "L" load menu.
struct LoadMenu {
    struct Entry {
        std::string label;
        std::filesystem::path path;
        bool isDir = false;
    };

    bool open = false;
    std::filesystem::path dir;
    std::vector<Entry> entries;
    int selected = 0;

    // Type-to-jump
    std::string typed;
    double typedAt = 0;

    void show(const std::filesystem::path& directory, const std::filesystem::path& select = {});
    void move(int delta);
    void jumpTo(const std::string& text, double now);
};
