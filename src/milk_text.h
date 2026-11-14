#pragma once

#include <filesystem>
#include <string>

// A .milk preset split into the parts MilkDrop 2 mash-ups exchange.
struct MilkParts {
    std::string base;  // everything except the warp/comp shaders
    std::string warp;  // warp_N= lines + PSVERSION_WARP
    std::string comp;  // comp_N= lines + PSVERSION_COMP
};

std::string readTextFile(const std::filesystem::path& file);
MilkParts splitMilk(const std::string& text);
std::string joinMilk(const MilkParts& parts);
