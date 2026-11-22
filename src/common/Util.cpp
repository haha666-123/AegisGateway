#include "common/Util.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <random>

namespace aegis {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

thread_local std::mt19937_64 g_rng{std::random_device{}()};

}  // namespace

TimePoint steady_now() noexcept { return SteadyClock::now(); }

int64_t to_millis(std::chrono::milliseconds d) noexcept { return static_cast<int64_t>(d.count()); }

std::string format_local_time(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;

    const auto secs = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();

    const std::time_t tt = system_clock::to_time_t(secs);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif

    std::array<char, 32> buf{};
    const auto n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                                 tmv.tm_min, tmv.tm_sec, static_cast<int>(millis));
    if (n <= 0) return {};
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

std::string to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

std::string_view trim(std::string_view text) noexcept {
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char a = lhs[i];
        char b = rhs[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::vector<std::string_view> split(std::string_view text, char delim) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string random_hex(std::size_t bytes) {
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto byte = static_cast<uint8_t>(g_rng() & 0xFFU);
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

uint64_t fnv1a64(std::string_view text) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    for (const char c : text) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string sanitize_for_log(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            switch (c) {
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", uc);
                    out += buf;
                }
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string human_bytes(uint64_t bytes) {
    constexpr std::array<const char*, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, kUnits[unit]);
    return buf;
}

}  // namespace aegis
