#include "preset_library.h"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

PresetLibrary::PresetLibrary(fs::path root, fs::path ratingsFile)
    : root_(std::move(root)), ratingsFile_(std::move(ratingsFile)) {
    loadRatings();
    setDirectory(root_);
}

void PresetLibrary::setDirectory(const fs::path& dir) {
    dir_ = dir;
    pool_.clear();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().extension() == ".milk") pool_.push_back(it->path());
    }
    std::sort(pool_.begin(), pool_.end());
}

std::optional<fs::path> PresetLibrary::pickRandom() {
    if (pool_.empty()) return std::nullopt;

    // MilkDrop: the rating is the weight; rating 0 = never picked, unless all are 0.
    std::vector<double> weights;
    weights.reserve(pool_.size());
    double total = 0;
    for (const auto& p : pool_) {
        weights.push_back(rating(p));
        total += weights.back();
    }
    if (total <= 0) {
        return pool_[std::uniform_int_distribution<size_t>(0, pool_.size() - 1)(rng_)];
    }
    std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
    return pool_[dist(rng_)];
}

std::optional<fs::path> PresetLibrary::pickNext(const fs::path& current) {
    if (pool_.empty()) return std::nullopt;

    if (random) {
        for (int attempt = 0; attempt < 10; attempt++) {
            auto p = pickRandom();
            if (!p || *p != current || pool_.size() == 1) return p;
        }
        return pickRandom();
    }

    const auto it = std::find(pool_.begin(), pool_.end(), current);
    if (it == pool_.end() || it + 1 == pool_.end()) return pool_.front();
    return *(it + 1);
}

std::string PresetLibrary::key(const fs::path& file) const {
    std::error_code ec;
    const auto rel = fs::relative(file, root_, ec);
    if (!ec && !rel.empty() && rel.native()[0] != '.') return rel.generic_u8string();
    return file.generic_u8string();
}

int PresetLibrary::rating(const fs::path& file) const {
    const auto it = ratings_.find(key(file));
    return it == ratings_.end() ? kDefaultRating : it->second;
}

void PresetLibrary::setRating(const fs::path& file, int rating) {
    ratings_[key(file)] = std::clamp(rating, 0, 5);
    saveRatings();
}

void PresetLibrary::loadRatings() {
    std::ifstream in(ratingsFile_);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.rfind('=');
        if (eq == std::string::npos || eq == 0) continue;
        try {
            ratings_[line.substr(0, eq)] = std::clamp(std::stoi(line.substr(eq + 1)), 0, 5);
        } catch (const std::exception&) {
        }
    }
}

void PresetLibrary::saveRatings() const {
    std::ofstream out(ratingsFile_);
    out << "# Preset ratings 0-5 (0 = never picked randomly). Written by feishin-visualizer.\n";
    for (const auto& [k, v] : ratings_) out << k << "=" << v << "\n";
}
