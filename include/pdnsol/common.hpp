#pragma once
#include <nlohmann/json.hpp>

#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/id_string.hpp"

using Json = nlohmann::ordered_json;

namespace pdnsol {
struct Node;

using IndexType  = int64_t;
using ScalarType = double;
using Tick       = FPN::Rep;

const IdString DEFAULT("default");
const IdString GND("GND");
} // namespace pdnsol