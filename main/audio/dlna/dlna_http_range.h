#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace DlnaHttpRange {

enum class Result {
    kValid,
    // Ignore malformed or unsupported Range fields and send the full entity.
    kIgnore,
    // A syntactically valid byte range has no satisfiable byte in the entity.
    kUnsatisfiable,
};

inline bool ParseSize(const std::string& text, size_t& value) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (c < '0' || c > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed > static_cast<unsigned long long>(SIZE_MAX)) {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

inline Result Parse(const std::string& header, size_t total,
                    size_t& first, size_t& last) {
    // Unsupported units and multi-range requests are deliberately ignored.
    // This server supports one byte range, not multipart/byteranges.
    if (header.compare(0, 6, "bytes=") != 0 || header.find(',') != std::string::npos) {
        return Result::kIgnore;
    }

    const std::string spec = header.substr(6);
    const size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find('-', dash + 1) != std::string::npos) {
        return Result::kIgnore;
    }

    const std::string left = spec.substr(0, dash);
    const std::string right = spec.substr(dash + 1);
    if (left.empty()) {
        size_t suffix = 0;
        if (!ParseSize(right, suffix)) return Result::kIgnore;
        if (suffix == 0 || total == 0) return Result::kUnsatisfiable;
        first = suffix >= total ? 0 : total - suffix;
        last = total - 1;
        return Result::kValid;
    }

    if (!ParseSize(left, first)) return Result::kIgnore;
    if (right.empty()) {
        if (first >= total) return Result::kUnsatisfiable;
        last = total - 1;
        return Result::kValid;
    }

    if (!ParseSize(right, last) || last < first) return Result::kIgnore;
    if (first >= total) return Result::kUnsatisfiable;
    last = std::min(last, total - 1);
    return Result::kValid;
}

}  // namespace DlnaHttpRange
