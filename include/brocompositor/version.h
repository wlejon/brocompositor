#pragma once

#include "brocompositor/export.h"
#include <string>

#define BRO_COMPOSITOR_VERSION_MAJOR 0
#define BRO_COMPOSITOR_VERSION_MINOR 1
#define BRO_COMPOSITOR_VERSION_PATCH 0
#define BRO_COMPOSITOR_VERSION_STRING "0.1.0"

namespace brocompositor {

BROCOMPOSITOR_API std::string version_string();
BROCOMPOSITOR_API int version_major();
BROCOMPOSITOR_API int version_minor();
BROCOMPOSITOR_API int version_patch();

} // namespace brocompositor
