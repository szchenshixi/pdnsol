#ifndef FIXED_POINT_NUMBER_HPP_
#define FIXED_POINT_NUMBER_HPP_

#include <cstdint>

/**
 * @file FixedPointNumber.hpp
 *
 * @brief Simple fixed-point utility functions.
 *
 * A "fixed-point number" here means a real value x is represented as:
 *
 *   stored_value = static_cast<Rep>(x * scale)
 *   x            = static_cast<double>(stored_value) / scale
 *
 * where `scale` is a single global scaling factor shared by all conversions
 * in this process. The scale is configured via initFixedPointNumberScale().
 */

namespace pdnsol::FPN {

/// Underlying integer type used to store fixed-point representations.
using Rep = std::int64_t;

/**
 * @brief Initialize the global fixed-point scaling factor.
 *
 * After initialization, the scale is used by all calls to toRep() and
 * fromRep().
 *
 * If this function is never called, the first call to toRep() or fromRep()
 * will lazily initialize the scale to a default value (see implementation).
 *
 * @param scale
 *   Positive scaling factor. The logical value x is represented as:
 *     stored = static_cast<Rep>(x * scale)
 *     x      = static_cast<double>(stored) / scale
 *
 * @note
 *   - Passing 0 is an error and leaves the scale unchanged.
 *   - Calling this function more than once with a different value is an error.
 *   - Calling it multiple times with the same value is allowed (no-op).
 */
void initFixedPointNumberScale(std::uint32_t scale);

/**
 * @brief Convert an integral fixed-point representation to double.
 *
 * @param rep
 *   Integral representation previously produced by toRep().
 *
 * @return
 *   The corresponding double value: static_cast<double>(rep) / scale.
 */
[[nodiscard]] double fromRep(Rep rep);

/**
 * @brief Convert a double to integral fixed-point representation.
 *
 * @param value
 *   The value to be quantized.
 *
 * @return
 *   The nearest representable integer representation:
 *   static_cast<Rep>(std::llround(value * scale)).
 *
 * @note
 *   On overflow, the implementation may clamp to the representable range
 *   and log an error.
 */
[[nodiscard]] Rep toRep(double value);

} // namespace pdnsol::FPN
#endif // FIXED_POINT_NUMBER_HPP_