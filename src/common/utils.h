#pragma once

#include <string>
#include <cstdint>
#include <cctype>

namespace cfrp {
namespace common {

inline int64_t ParseBandwidth(const std::string& s) {
    if (s.empty()) return 0;
    std::string val_s = s;
    char unit = std::toupper(val_s.back());
    int64_t multiplier = 1;
    if (unit == 'K') {
        multiplier = 1024;
        val_s.pop_back();
    } else if (unit == 'M') {
        multiplier = 1024 * 1024;
        val_s.pop_back();
    } else if (unit == 'G') {
        multiplier = 1024 * 1024 * 1024;
        val_s.pop_back();
    }
    try {
        return std::stoll(val_s) * multiplier;
    } catch (...) {
        return 0;
    }
}

} // namespace common
} // namespace cfrp
