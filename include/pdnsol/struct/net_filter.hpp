#pragma once

#include <string>
#include <string_view>

#include "pdnsol/io/parser_config.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {

namespace detail {

inline std::string_view stripOptionalQuotesView(std::string_view s) noexcept {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

inline bool listContainsId(const std::vector<IdString>& names,
                           const IdString&              name) {
    for (IdString n : names) {
        if (n == name) return true;
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
    std::vector<IdString> includes; // e.g. {"VDD", "VDD_MEM"}
    std::vector<IdString> excludes; // e.g. {"VSS"}

    bool includePower  = true;
    bool includeGround = true;

    NetFilter() = default;
    NetFilter(const NetFilterConfig& config)
        : includes(config.includeNets)
        , excludes(config.excludeNets)
        , includePower(config.includePower)
        , includeGround(config.includeGround) {}

    bool isAllowAll() const noexcept {
        return includes.empty() && excludes.empty() && includePower &&
               includeGround;
    }

    // Name-only filtering (no type info needed)
    bool allowsName(const IdString& netName) const {
        if (!excludes.empty() && detail::listContainsId(excludes, netName)) {
            return false;
        }
        if (!includes.empty() && !detail::listContainsId(includes, netName)) {
            return false;
        }
        return true;
    }

    // True only when user explicitly listed this net in `includes`
    bool explicitlyIncludesName(const IdString& netName) const {
        if (includes.empty()) return false;
        return detail::listContainsId(includes, netName);
    }

    // Full filtering with type info
    bool allows(const IdString& netName, bool isPower, bool isGround) const {
        if (isPower && !includePower) return false;
        if (isGround && !includeGround) return false;
        return allowsName(netName);
    }
};

} // namespace pdnsol