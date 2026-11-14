#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Preset pool (current directory, recursive), MilkDrop-style ratings 0-5 and
// random (rating-weighted) or sequential selection.
class PresetLibrary {
public:
    PresetLibrary(std::filesystem::path root, std::filesystem::path ratingsFile);

    void setDirectory(const std::filesystem::path& dir);
    const std::filesystem::path& directory() const { return dir_; }
    const std::filesystem::path& root() const { return root_; }
    size_t size() const { return pool_.size(); }

    bool random = true;

    // Next preset after `current` (random: weighted by rating, never the same one twice in a row).
    std::optional<std::filesystem::path> pickNext(const std::filesystem::path& current);
    std::optional<std::filesystem::path> pickRandom();

    int rating(const std::filesystem::path& file) const;
    void setRating(const std::filesystem::path& file, int rating);

private:
    std::string key(const std::filesystem::path& file) const;
    void loadRatings();
    void saveRatings() const;

    std::filesystem::path root_;
    std::filesystem::path dir_;
    std::filesystem::path ratingsFile_;
    std::vector<std::filesystem::path> pool_;
    std::map<std::string, int> ratings_;
    std::mt19937 rng_{std::random_device{}()};
};

static constexpr int kDefaultRating = 3;
