/**
 * @file browser_trim.cpp
 * @brief Automatic headless browser memory trimming and argument injection.
 */

#include "virmux/browser_trim.hpp"

#include <string_view>

namespace virmux {

bool is_browser_binary(const char *path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::string_view p(path);
    return p.find("chrome") != std::string_view::npos ||
           p.find("chromium") != std::string_view::npos ||
           p.find("headless_shell") != std::string_view::npos;
}

std::vector<std::string> inject_slim_flags(char *const argv[]) {
    std::vector<std::string> result;
    if (argv == nullptr || argv[0] == nullptr) {
        return result;
    }

    // 1. Keep argv[0]
    result.emplace_back(argv[0]);

    // 2. Insert slim flags immediately after argv[0]
    result.emplace_back("--disable-gpu");
    result.emplace_back("--single-process");
    result.emplace_back("--disable-software-rasterizer");

    // 3. Append remaining original argv elements
    for (size_t i = 1; argv[i] != nullptr; ++i) {
        result.emplace_back(argv[i]);
    }

    return result;
}

} // namespace virmux
