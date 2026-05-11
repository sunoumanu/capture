// ms2109_common.hpp
// Shared types, utility functions, and constants for the MS2109 capture pipeline.

#ifndef MS2109_COMMON_HPP
#define MS2109_COMMON_HPP

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cctype>

using clk = std::chrono::steady_clock;

struct Mode {
    int width;
    int height;
    int fps;
};

inline Mode parseMode(const std::string& s) {
    if (s == "1080p30") return {1920, 1080, 30};
    if (s == "720p60")  return {1280, 720, 60};
    if (s == "720p30")  return {1280, 720, 30};
    if (s == "480p30")  return {640, 480, 30};
    std::cerr << "Unknown mode '" << s << "', falling back to 1080p30.\n";
    return {1920, 1080, 30};
}

inline bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

inline std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string fourccToString(int f) {
    char s[5] = { static_cast<char>(f & 0xFF),
                  static_cast<char>((f >> 8) & 0xFF),
                  static_cast<char>((f >> 16) & 0xFF),
                  static_cast<char>((f >> 24) & 0xFF),
                  0 };
    return std::string(s);
}

#endif // MS2109_COMMON_HPP
