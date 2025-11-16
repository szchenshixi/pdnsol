#include <unordered_map>

#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
struct Node;

using IndexType = int64_t;
using ScalarType = double;

using IntMap = std::unordered_map<IdString, IndexType, IdString::Hash>;
using DoubleMap = std::unordered_map<IdString, ScalarType, IdString::Hash>;
using IdStringMap = std::unordered_map<IdString, IdString, IdString::Hash>;
using NodeMap = std::unordered_map<IdString, Node, IdString::Hash>;

using Tick = FPN::Rep;

} // namespace pdnsol