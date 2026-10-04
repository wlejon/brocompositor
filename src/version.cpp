#include "brocompositor/version.h"

namespace brocompositor {

std::string version_string() {
    return BRO_COMPOSITOR_VERSION_STRING;
}

int version_major() {
    return BRO_COMPOSITOR_VERSION_MAJOR;
}

int version_minor() {
    return BRO_COMPOSITOR_VERSION_MINOR;
}

int version_patch() {
    return BRO_COMPOSITOR_VERSION_PATCH;
}

} // namespace brocompositor
