#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
IdString::Pool& IdString::pool() {
    static Pool p;
    return p;
}

uint32_t IdString::internGlobal(std::string_view sv) {
    auto& p = pool();
    std::lock_guard<std::mutex> lock(p.mMu);
    if (auto it = p.mMap.find(sv); it != p.mMap.end()) { return it->second; }

    std::string owned{sv};
    uint32_t id = static_cast<uint32_t>(p.mPool.size());
    p.mPool.push_back(owned);
    p.mMap.emplace(std::move(owned), id);
    return id;
}

uint32_t IdString::lookupGlobal(std::string_view sv) {
    auto& p = pool();
    std::lock_guard<std::mutex> lock(p.mMu);
    if (auto it = p.mMap.find(sv); it != p.mMap.end()) { return it->second; }
    return kInvalid;
}

const std::string& IdString::resolveGlobal(uint32_t id) {
    auto& p = pool();
    if (id == kInvalid || id >= p.mPool.size()) { return getInvalidStr(); }
    return p.mPool[id];
}
} // namespace hdl