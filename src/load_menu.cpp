#include "load_menu.h"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

void LoadMenu::show(const fs::path& directory, const fs::path& select) {
    open = true;
    dir = directory;
    entries.clear();
    selected = 0;
    typed.clear();

    if (dir.has_parent_path() && dir.parent_path() != dir) entries.push_back({"..", dir.parent_path(), true});

    std::vector<Entry> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            dirs.push_back({"[" + it->path().filename().u8string() + "]", it->path(), true});
        } else if (it->path().extension() == ".milk") {
            files.push_back({it->path().stem().u8string(), it->path(), false});
        }
    }
    const auto byLabel = [](const Entry& a, const Entry& b) { return lower(a.label) < lower(b.label); };
    std::sort(dirs.begin(), dirs.end(), byLabel);
    std::sort(files.begin(), files.end(), byLabel);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());

    for (size_t i = 0; i < entries.size(); i++) {
        if (!select.empty() && entries[i].path == select) selected = static_cast<int>(i);
    }
}

void LoadMenu::move(int delta) {
    if (entries.empty()) return;
    selected = std::clamp(selected + delta, 0, static_cast<int>(entries.size()) - 1);
}

void LoadMenu::jumpTo(const std::string& text, double now) {
    if (now - typedAt > 1.0) typed.clear();
    typedAt = now;
    typed += lower(text);

    for (size_t i = 0; i < entries.size(); i++) {
        std::string label = lower(entries[i].label);
        if (!label.empty() && label[0] == '[') label.erase(0, 1);
        if (label.compare(0, typed.size(), typed) == 0) {
            selected = static_cast<int>(i);
            return;
        }
    }
}
