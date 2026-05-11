/**
 * NavCVars — implementation. See header for the manifest.
 */

#include "NavCVars.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

namespace AnchorNavCVars {

bool IsMasterEnabled() {
    return CVarGetInteger(kEnabled, 0) != 0;
}

bool IsFeatureEnabled(const char* featureCVar) {
    if (CVarGetInteger(kEnabled, 0) == 0) return false;
    if (featureCVar == nullptr) return false;
    return CVarGetInteger(featureCVar, 0) != 0;
}

} // namespace AnchorNavCVars
