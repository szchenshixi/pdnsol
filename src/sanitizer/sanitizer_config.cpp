#include "pdnsol/sanitizer/sanitizer_config.hpp"

#include <unordered_set>

#include "pdnsol/utils/logging.hpp"

namespace pdnsol {
bool integrityCheck(const Json& configJ, const std::string& filePath) {
    bool result = true;
#ifndef NDEBUG
    // Alias for filesystem if you want
    namespace fs = std::filesystem;

    // -------------------------------------------------------------------------
    // Helper lambdas
    // -------------------------------------------------------------------------

    // Require that an object field exists and is itself a JSON object.
    auto requireObject = [&](const Json&        parent,
                             const char*        key,
                             const std::string& context) -> const Json* {
        if (!parent.contains(key)) {
            PDN_ERROR("Cannot find '%s' section in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        const Json& child = parent.at(key);
        if (!child.is_object()) {
            PDN_ERROR("'%s' in %s must be a JSON object (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        return &child;
    };

    // Require that a field exists and is an array (optionally non-empty).
    auto requireArray = [&](const Json&        parent,
                            const char*        key,
                            const std::string& context,
                            bool mustBeNonEmpty = true) -> const Json* {
        if (!parent.contains(key)) {
            PDN_ERROR("Cannot find '%s' array in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        const Json& arr = parent.at(key);
        if (!arr.is_array()) {
            PDN_ERROR("Field '%s' in %s must be an array (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return nullptr;
        }
        if (mustBeNonEmpty && arr.empty()) {
            PDN_ERROR("Array '%s' in %s must not be empty (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            // We still return it so caller can iterate if desired.
        }
        return &arr;
    };

    // Require that a field exists, is a string, and is not empty.
    auto requireNonEmptyString =
      [&](const Json&        obj,
          const char*        key,
          const std::string& context) -> std::string {
        if (!obj.contains(key)) {
            PDN_ERROR("Missing '%s' in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return std::string{};
        }

        const Json& jv = obj.at(key);
        if (!jv.is_string()) {
            PDN_ERROR("Field '%s' in %s must be a string (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return std::string{};
        }

        std::string value = jv.get<std::string>();
        if (value.empty()) {
            PDN_ERROR(
              "Field '%s' in %s must not be an empty string (context: %s)",
              key,
              filePath.c_str(),
              context.c_str());
            result = false;
        }
        return value;
    };

    // Require that a field exists, is numeric, and >= 0.0.
    // If it's exactly 0.0, only emit a warning.
    auto requireNonNegativeNumber = [&](const Json&        obj,
                                        const char*        key,
                                        const std::string& context) -> double {
        if (!obj.contains(key)) {
            PDN_ERROR("Missing '%s' in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return 0.0;
        }

        const Json& jv = obj.at(key);
        if (!jv.is_number()) {
            PDN_ERROR("Field '%s' in %s must be a number (context: %s)",
                      key,
                      filePath.c_str(),
                      context.c_str());
            result = false;
            return 0.0;
        }

        double value = jv.get<double>();
        if (value < 0.0) {
            PDN_ERROR("Field '%s' in %s must be >= 0.0 (got %g) (context: %s)",
                      key,
                      filePath.c_str(),
                      value,
                      context.c_str());
            result = false;
        } else if (value == 0.0) {
            PDN_WARN("Field '%s' in %s is 0.0 (context: %s)",
                     key,
                     filePath.c_str(),
                     context.c_str());
        }
        return value;
    };

    // Require that a field exists, is a string, is not empty, and points to an
    // existing file on disk.
    auto requireExistingFile =
      [&](const Json& obj, const char* key, const std::string& context) {
          std::string path = requireNonEmptyString(obj, key, context);
          if (!path.empty()) {
              if (!fs::exists(path)) {
                  PDN_ERROR(
                    "File '%s' referenced by '%s' does not exist (config: %s)",
                    path.c_str(),
                    context.c_str(),
                    filePath.c_str());
                  result = false;
              }
          }
      };

    // -------------------------------------------------------------------------
    // Top-level sections
    // -------------------------------------------------------------------------

    const Json* fileJ = requireObject(configJ, "file", "top-level");
    const Json* simJ  = requireObject(configJ, "simulation", "top-level");
    const Json* techJ = requireObject(configJ, "tech", "top-level");

    // -------------------------------------------------------------------------
    // File Section
    // -------------------------------------------------------------------------
    if (fileJ) {
        const std::string context = "file section";

        // "def_path": must exist, non-empty string, and file must exist
        requireExistingFile(*fileJ, "def_path", "file.def_path");

        // "current_src_path": same
        requireExistingFile(
          *fileJ, "current_src_path", "file.current_src_path");

        // "voltage_src_path": same
        requireExistingFile(
          *fileJ, "voltage_src_path", "file.voltage_src_path");
    }

    // -------------------------------------------------------------------------
    // Technology Section
    // -------------------------------------------------------------------------
    std::unordered_set<std::string> metalLayerNames;

    if (techJ) {
        // ------------------------- metal_layers -----------------------------
        const Json* metalsArr =
          requireArray(*techJ, "metal_layers", "tech.metal_layers");
        if (metalsArr) {
            for (std::size_t i = 0; i < metalsArr->size(); ++i) {
                const Json& m = (*metalsArr)[i];
                std::string context =
                  "tech.metal_layers[" + std::to_string(i) + "]";

                if (!m.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.metal_layers' must be an "
                              "object (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string name =
                  requireNonEmptyString(m, "name", context + ".name");
                if (!name.empty()) {
                    metalLayerNames.insert(name);
                }

                requireNonNegativeNumber(m,
                                         "resistivity_ohm_x_um",
                                         context + ".resistivity_ohm_x_um");
                requireNonNegativeNumber(
                  m, "thickness_um", context + ".thickness_um");
            }
        }

        // ------------------------- vias -------------------------------------
        const Json* viasArr = requireArray(*techJ, "vias", "tech.vias");
        if (viasArr) {
            for (std::size_t i = 0; i < viasArr->size(); ++i) {
                const Json& v       = (*viasArr)[i];
                std::string context = "tech.vias[" + std::to_string(i) + "]";

                if (!v.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.vias' must be an object "
                              "(config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string viaName =
                  requireNonEmptyString(v, "name", context + ".name");
                std::string bottom = requireNonEmptyString(
                  v, "bottom_layer", context + ".bottom_layer");
                std::string top = requireNonEmptyString(
                  v, "top_layer", context + ".top_layer");

                requireNonNegativeNumber(
                  v, "resistance_ohm", context + ".resistance_ohm");

                // Cross-check that via layers exist in metal_layers
                if (!bottom.empty() && !metalLayerNames.empty() &&
                    metalLayerNames.find(bottom) == metalLayerNames.end()) {
                    PDN_ERROR("Via '%s' references unknown bottom_layer '%s' "
                              "(config: %s)",
                              viaName.c_str(),
                              bottom.c_str(),
                              filePath.c_str());
                    result = false;
                }
                if (!top.empty() && !metalLayerNames.empty() &&
                    metalLayerNames.find(top) == metalLayerNames.end()) {
                    PDN_ERROR("Via '%s' references unknown top_layer '%s' "
                              "(config: %s)",
                              viaName.c_str(),
                              top.c_str(),
                              filePath.c_str());
                    result = false;
                }
            }
        }

        // ------------------------- tsvs -------------------------------------
        const Json* tsvsArr = requireArray(*techJ, "tsvs", "tech.tsvs");
        if (tsvsArr) {
            for (std::size_t i = 0; i < tsvsArr->size(); ++i) {
                const Json& v       = (*tsvsArr)[i];
                std::string context = "tech.tsvs[" + std::to_string(i) + "]";

                if (!v.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.tsvs' must be an object "
                              "(config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string tsvName =
                  requireNonEmptyString(v, "name", context + ".name");
                std::string bottom = requireNonEmptyString(
                  v, "bottom_layer", context + ".bottom_layer");
                std::string top = requireNonEmptyString(
                  v, "top_layer", context + ".top_layer");

                requireNonNegativeNumber(
                  v, "resistance_ohm", context + ".resistance_ohm");

                // Cross-check that via layers exist in metal_layers
                if (!bottom.empty() && !metalLayerNames.empty() &&
                    metalLayerNames.find(bottom) == metalLayerNames.end()) {
                    PDN_ERROR("Tsv '%s' references unknown bottom_layer '%s' "
                              "(config: %s)",
                              tsvName.c_str(),
                              bottom.c_str(),
                              filePath.c_str());
                    result = false;
                }
                if (!top.empty() && !metalLayerNames.empty() &&
                    metalLayerNames.find(top) == metalLayerNames.end()) {
                    PDN_ERROR("Tsv '%s' references unknown top_layer '%s' "
                              "(config: %s)",
                              tsvName.c_str(),
                              top.c_str(),
                              filePath.c_str());
                    result = false;
                }
            }
        }

        // -------------------- powerNets -------------------------------------
        const Json* powerArr =
          requireArray(*techJ, "power_nets", "tech.power_nets");
        if (powerArr) {
            for (std::size_t i = 0; i < powerArr->size(); ++i) {
                const Json& pn = (*powerArr)[i];
                std::string context =
                  "tech.power_nets[" + std::to_string(i) + "]";

                if (!pn.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.power_nets' must be an "
                              "object (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                requireNonEmptyString(pn, "name", context + ".name");
                requireNonNegativeNumber(
                  pn, "voltage_volt", context + ".voltage_volt");
                requireNonNegativeNumber(pn,
                                         "package_resistance_ohm",
                                         context + ".package_resistance_ohm");
            }
        }

        // ------------------- groundNets -------------------------------------
        const Json* groundArr =
          requireArray(*techJ, "ground_nets", "tech.ground_nets");
        if (groundArr) {
            for (std::size_t i = 0; i < groundArr->size(); ++i) {
                const Json& gn = (*groundArr)[i];
                std::string context =
                  "tech.ground_nets[" + std::to_string(i) + "]";

                if (!gn.is_object()) {
                    PDN_ERROR("Element %zu of 'tech.ground_nets' must be an "
                              "object (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                requireNonEmptyString(gn, "name", context + ".name");
                requireNonNegativeNumber(
                  gn, "voltage_volt", context + ".voltage_volt");
                requireNonNegativeNumber(gn,
                                         "package_resistance_ohm",
                                         context + ".package_resistance_ohm");
            }
        }

        // ------------------- layer_order ------------------------------------
        const Json* orderArr =
          requireArray(*techJ, "layer_order", "tech.layer_order");
        if (orderArr) {
            for (std::size_t i = 0; i < orderArr->size(); ++i) {
                const Json& lv = (*orderArr)[i];
                std::string context =
                  "tech.layer_order[" + std::to_string(i) + "]";

                if (!lv.is_string()) {
                    PDN_ERROR("Element %zu of 'tech.layer_order' must be a "
                              "string (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                std::string layer = lv.get<std::string>();
                if (layer.empty()) {
                    PDN_ERROR("Element %zu of 'tech.layer_order' must not be "
                              "empty (config: %s)",
                              i,
                              filePath.c_str());
                    result = false;
                    continue;
                }

                if (!metalLayerNames.empty() &&
                    metalLayerNames.find(layer) == metalLayerNames.end()) {
                    PDN_ERROR("Layer '%s' in 'tech.layer_order' is not "
                              "defined in 'tech.metal_layers' (config: %s)",
                              layer.c_str(),
                              filePath.c_str());
                    result = false;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Simulation Section
    // -------------------------------------------------------------------------
    if (simJ) {
        // Integers in JSON are also numbers, so requireNonNegativeNumber is
        // OK.

        // grid_Nx >= 0.0, warn if 0
        // requireNonNegativeNumber(*simJ, "grid_Nx", "simulation.grid_Nx");

        // grid_Ny >= 0.0, warn if 0
        // requireNonNegativeNumber(*simJ, "grid_Ny", "simulation.grid_Ny");

        // Enforce it to by within the defined metal layers
        const char* key = "bump_layer";
        if (!simJ->contains(key)) {
            PDN_ERROR("Missing '%s' in %s (context: %s)",
                      key,
                      filePath.c_str(),
                      "simulation.bump_layer");
            result = false;
        } else {
            const Json& jv = simJ->at(key);
            if (!jv.is_string()) {
                PDN_ERROR("Field '%s' in %s must be a string (context: %s)",
                          key,
                          filePath.c_str(),
                          "simulation.bump_layer");
                result = false;
            } else {
                std::string bumpLayer = jv.get<std::string>();
                if (bumpLayer.empty()) {
                    PDN_ERROR("Field '%s' in %s must not be an empty string "
                              "(context: %s)",
                              key,
                              filePath.c_str(),
                              "simulation.bump_layer");
                    result = false;
                } else if (!metalLayerNames.empty() &&
                           metalLayerNames.find(bumpLayer) ==
                             metalLayerNames.end()) {
                    PDN_ERROR(
                      "'bump_layer' references unknown metal layer '%s' "
                      "(config: %s)",
                      bumpLayer.c_str(),
                      filePath.c_str());
                    result = false;
                }
            }
        }
    }
#endif // NDEBUG
    return result;
}
} // namespace pdnsol