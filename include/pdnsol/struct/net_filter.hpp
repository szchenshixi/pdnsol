#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {

namespace detail {

inline std::string_view stripOptionalQuotesView(std::string_view s) noexcept {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

inline bool listContainsId(const std::vector<std::string>& names,
                           const IdString&                 id) {
    for (const std::string& raw : names) {
        const std::string_view sv = stripOptionalQuotesView(raw);
        // Avoid assuming IdString has a string_view ctor:
        if (sv == id.str()) {
            return true;
        }
    }
    return false;
}

} // namespace detail

// Shared net filter used by both PDN graph builder and circuit decorator
//
// Semantics:
// - If include is empty => include all nets (subject to exclude + type flags)
// - exclude is applied after include.
// - includePower/includeGround can be used to keep only power or only ground
struct NetFilter {
    std::vector<std::string> include; // e.g. {"VDD", "\"VDDQ\""}
    std::vector<std::string> exclude; // e.g. {"VSS"}

    bool includePower  = true;
    bool includeGround = true;

    bool isAllowAll() const noexcept {
        return include.empty() && exclude.empty() && includePower &&
               includeGround;
    }

    // Name-only filtering (no type info needed).
    bool allowsName(const IdString& netName) const {
        if (!exclude.empty() && detail::listContainsId(exclude, netName)) {
            return false;
        }
        if (!include.empty() && !detail::listContainsId(include, netName)) {
            return false;
        }
        return true;
    }

    // True only when user explicitly listed this net in `include`.
    bool explicitlyIncludesName(const IdString& netName) const {
        if (include.empty()) return false;
        return detail::listContainsId(include, netName);
    }

    // Full filtering with type info.
    bool allows(const IdString& netName, bool isPower, bool isGround) const {
        if (isPower && !includePower) return false;
        if (isGround && !includeGround) return false;
        return allowsName(netName);
    }
};

} // namespace pdnsol