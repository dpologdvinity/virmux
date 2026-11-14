#pragma once

/**
 * @file version.hpp
 * @brief Core version definitions for virmux.
 */

namespace virmux {

/**
 * @brief Returns the semantic version string of virmux.
 * @return Semantic version string (e.g. "0.1.0").
 */
[[nodiscard]] const char* version_string() noexcept;

} // namespace virmux
