#include "pdnsol/utils/fixed_point_number.hpp"

#include <nlohmann/json.hpp>

using Json = nlohmann::ordered_json;

namespace pdnsol {
struct Node;

using IndexType  = int64_t;
using ScalarType = double;
using Tick = FPN::Rep;
} // namespace pdnsol