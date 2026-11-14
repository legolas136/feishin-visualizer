#include "milk_text.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace {

bool isNumberedKey(const std::string& line, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    if (line.compare(0, n, prefix) != 0) return false;
    size_t i = n;
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i]))) return false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) i++;
    return i < line.size() && line[i] == '=';
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

}  // namespace

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

MilkParts splitMilk(const std::string& text) {
    MilkParts parts;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string* target = &parts.base;
        if (isNumberedKey(line, "warp_") || startsWith(line, "PSVERSION_WARP=")) target = &parts.warp;
        else if (isNumberedKey(line, "comp_") || startsWith(line, "PSVERSION_COMP=")) target = &parts.comp;
        target->append(line).push_back('\n');
    }
    return parts;
}

std::string joinMilk(const MilkParts& parts) {
    return parts.base + parts.warp + parts.comp;
}
