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
    auto slash = p.rfind('/');
    std::string_view name = (slash != std::string_view::npos) ? p.substr(slash + 1) : p;

    if (name.empty()) {
        return false;
    }

    // Explicitly exclude browser helper processes and dev tools
    if (name.starts_with("chromedriver") ||
        name.starts_with("chrome-sandbox")) {
        return false;
    }

    // Exact matches for standard headless browser binaries
    if (name == "chrome" || name == "chromium" || name == "headless_shell") {
        return true;
    }

    // Common distribution package binary names and prefixes
    if (name == "google-chrome" ||
        name.starts_with("google-chrome-") ||
        name == "chromium-browser" ||
        name.starts_with("chromium-") ||
        name.starts_with("chrome-")) {
        return true;
    }

    return false;
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
