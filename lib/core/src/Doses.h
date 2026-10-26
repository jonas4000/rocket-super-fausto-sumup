#pragma once

#include "Ports.h"

namespace grinder {

// Factory defaults. Price and grind time are changed in the settings screen and
// persisted via SettingsStore, which overwrites these on boot.
inline Dose gDoses[] = {
    {"Single", 100, 2600, 0, "single-spout", 1},
    {"Double", 130, 3900, 0, "double-spout", 2},
};
inline constexpr size_t kDoseCount = sizeof(gDoses) / sizeof(gDoses[0]);

}  // namespace grinder
