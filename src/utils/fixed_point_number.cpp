#include "pdnsol/utils/fixed_point_number.hpp"

#include <cmath>  // std::llround
#include <limits> // std::numeric_limits

#include "pdnsol/utils/logging.hpp"

namespace pdnsol::FPN {

// -------------------------
// Internal state
// -------------------------

namespace {

/**
 * @brief Sentinel value meaning "scale not initialized yet".
 *
 * We reserve std::numeric_limits<uint32_t>::max() for "uninitialized".
 * Users should avoid using that specific value as a real scale.
 */
constexpr uint32_t kScaleUninitialized = std::numeric_limits<uint32_t>::max();

/**
 * @brief Default scaling factor used for lazy initialization.
 *
 * This is the scale that will be used if the user never calls
 * initFixedPointNumberScale().
 */
constexpr uint32_t kDefaultScale = 1000U;

/**
 * @brief Global scaling factor for fixed-point conversions.
 *
 * Lifetime:
 *   - Starts as kScaleUninitialized.
 *   - Set exactly once either by initFixedPointNumberScale() or lazily
 *     by getScale() on first use.
 *
 * Thread-safety:
 *   - As written, this is NOT thread-safe. If you need thread safety, make
 * this an std::atomic<uint32_t> and use appropriate memory ordering.
 */
uint32_t g_scale = kScaleUninitialized;

/**
 * @brief Internal helper to set the scale when it is still uninitialized.
 *
 * @param scale New scale (assumed to be validated beforehand).
 * @return true if scale was updated, false if it was already initialized.
 */
bool setScaleIfUninitialized(uint32_t scale) {
    if (g_scale != kScaleUninitialized) {
        return false;
    }
    g_scale = scale;
    return true;
}

/**
 * @brief Internal helper to obtain the current scale, with lazy
 * initialization.
 *
 * If the scale has not been initialized yet, this function performs a
 * lazy initialization to kDefaultScale.
 *
 * @return Current global scale (always > 0 on return).
 */
uint32_t getScale() {
    if (g_scale == kScaleUninitialized) {
        // Lazy initialization path. In a single-threaded program this is safe
        // In multi-threaded code, this would need synchronization
        if (setScaleIfUninitialized(kDefaultScale)) {
            PDN_INFO(
              "Fixed-Point number lazily initialized with scaling factor %u",
              kDefaultScale);
        }
    }
    return g_scale;
}

} // namespace

// -------------------------
// Public API
// -------------------------

void initFixedPointNumberScale(uint32_t scale) {
    if (scale == 0U) {
        PDN_ERROR(
          "Fixed-Point scale must be greater than 0; requested scale = 0");
        return;
    }

    if (g_scale == kScaleUninitialized) {
        g_scale = scale;
        PDN_INFO("Fixed-Point number initialized with scaling factor %u",
                 scale);
        return;
    }

    // Scale is already initialized at this point
    if (g_scale == scale) {
        // Idempotent re-initialization: allow silently or with a debug log
        PDN_INFO(
          "Fixed-Point scale already initialized to %u; repeated call ignored",
          scale);
    } else {
        // Conflicting initialization: log an error and keep the original
        // value
        PDN_ERROR(
          "Fixed-Point scale already initialized to %u; ignoring new value %u",
          g_scale,
          scale);
    }
}

double fromRep(Rep rep) {
    const uint32_t scale = getScale();

    // Convert integral representation back to double
    // Using double for both numerator and denominator to avoid integer
    // division
    return static_cast<double>(rep) / static_cast<double>(scale);
}

Rep toRep(double value) {
    const uint32_t scale = getScale();

    // Compute scaled value in double
    const double scaled = value * static_cast<double>(scale);

    // Optional: detect overflow before converting to Rep
    const double maxRepAsDouble =
      static_cast<double>(std::numeric_limits<Rep>::max());
    const double minRepAsDouble =
      static_cast<double>(std::numeric_limits<Rep>::min());

    if (scaled > maxRepAsDouble || scaled < minRepAsDouble) {
        PDN_ERROR(
          "Fixed-Point conversion overflow: value=%f, scale=%u, scaled=%f "
          "(Rep range [%f, %f])",
          value,
          scale,
          scaled,
          minRepAsDouble,
          maxRepAsDouble);

        // Clamp to representable range to avoid undefined behavior
        if (scaled > 0.0) {
            return std::numeric_limits<Rep>::max();
        } else {
            return std::numeric_limits<Rep>::min();
        }
    }

    // Round to nearest integer (ties to even) instead of truncating toward
    // zero. This is usually preferable for fixed-point quantization
    const long long rounded = std::llround(scaled);
    return static_cast<Rep>(rounded);
}

} // namespace pdnsol::FPN