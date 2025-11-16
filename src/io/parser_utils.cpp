#include "pdnsol/io/parser_utils.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace pdnsol {

std::string ltrim(const std::string& s) {
    std::size_t pos = 0;
    while (pos < s.size() &&
           std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    return s.substr(pos);
}

std::string rtrim(const std::string& s) {
    if (s.empty()) return s;
    std::size_t pos = s.size();
    while (pos > 0 && std::isspace(static_cast<unsigned char>(s[pos - 1]))) {
        --pos;
    }
    return s.substr(0, pos);
}

std::string trim(const std::string& s) { return rtrim(ltrim(s)); }

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool startsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitWhitespace(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inToken = false;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (inToken) {
                tokens.push_back(cur);
                cur.clear();
                inToken = false;
            }
        } else {
            cur.push_back(c);
            inToken = true;
        }
    }
    if (inToken) tokens.push_back(cur);
    return tokens;
}

std::vector<std::string> splitOnChar(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            if (i > start) {
                out.emplace_back(s.substr(start, i - start));
            } else {
                out.emplace_back(std::string{});
            }
            start = i + 1;
        }
    }
    return out;
}

// -------------------------
// PDN node name parsing
// -------------------------

ParsedNode parsePdNodeName(const std::string& raw, double coordToMeterScale) {
    ParsedNode out;

    // Map SPICE/SPEF-style ground representations to canonical "GND"
    if (raw == "0" || iequals(raw, "gnd")) {
        out.id = IdString("GND");
        return out;
    }

    out.id = IdString(raw);

    // Detect your PDN convention: n<net>_<x>_<y>
    if (raw.size() > 1 && (raw[0] == 'n' || raw[0] == 'N')) {
        std::string rest = raw.substr(1); // drop leading 'n'
        auto parts = splitOnChar(rest, '_');
        if (parts.size() == 3) {
            try {
                int32_t net = static_cast<int32_t>(std::stoi(parts[0]));
                double x = std::stod(parts[1]);
                double y = std::stod(parts[2]);
                out.netIndex = net;
                out.xMeters = x * coordToMeterScale;
                out.yMeters = y * coordToMeterScale;
            } catch (...) {
                // If parsing fails, leave netIndex/x/y as defaults.
            }
        }
    }

    return out;
}

bool isPackageNodeName(const std::string& name) {
    return !name.empty() && name[0] == '_';
}

} // namespace pdnsol