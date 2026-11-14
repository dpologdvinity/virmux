/**
 * @file version.cpp
 * @brief Core version reporting implementation for virmux.
 */

#include "virmux/version.hpp"

namespace virmux {

/**
 * @brief Returns the semantic version string of virmux.
 */
const char* version_string() noexcept {
    return "0.1.0";
}

} // namespace virmux
