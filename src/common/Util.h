#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegis {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

[[nodiscard]] TimePoint steady_now() noexcept;

[[nodiscard]] int64_t to_millis(std::chrono::milliseconds d) noexcept;

// "2026-11-22 18:30:00.123"
[[nodiscard]] std::string format_local_time(std::chrono::system_clock::time_point tp);

[[nodiscard]] std::string to_lower(std::string_view text);
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
[[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept;

// Splits on a single delimiter, keeping empty fields. No allocation for the
// views themselves.
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delim);

[[nodiscard]] std::string random_hex(std::size_t bytes);

// FNV-1a 64 bit, used for cache keys and request fingerprints.
[[nodiscard]] uint64_t fnv1a64(std::string_view text) noexcept;

// Replaces control characters so a value can be logged safely.
[[nodiscard]] std::string sanitize_for_log(std::string_view text);

[[nodiscard]] std::string human_bytes(uint64_t bytes);

}  // namespace aegis
