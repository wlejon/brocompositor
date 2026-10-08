#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/geometry.h"

#include <optional>
#include <ostream>
#include <vector>

namespace brocompositor {

inline std::ostream& operator<<(std::ostream& os, const Rect& r) {
    return os << "{" << r.x << "," << r.y << " " << r.width << "x" << r.height << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Point& p) {
    return os << "(" << p.x << "," << p.y << ")";
}
inline std::ostream& operator<<(std::ostream& os, const Size& s) { return os << s.width << "x" << s.height; }
inline std::ostream& operator<<(std::ostream& os, WindowState s) { return os << to_string(s); }

}  // namespace brocompositor

#ifdef __linux__
#include "brocompositor/linux/server_events.h"

namespace brocompositor::wl {

inline std::ostream& operator<<(std::ostream& os, LockState s) {
    switch (s) {
        case LockState::Unlocked: return os << "Unlocked";
        case LockState::Locked: return os << "Locked";
        case LockState::Abandoned: return os << "Abandoned";
    }
    return os << "LockState(" << uint32_t(s) << ")";
}

inline std::ostream& operator<<(std::ostream& os, PointerConstraintKind k) {
    return os << (k == PointerConstraintKind::Locked ? "Locked" : k == PointerConstraintKind::Confined ? "Confined"
                                                                                                         : "None");
}

}  // namespace brocompositor::wl
#endif

namespace std {

template <class T>
ostream& operator<<(ostream& os, const optional<T>& v) {
    if (!v) return os << "nullopt";
    using brocompositor::operator<<;
    return os << *v;
}

template <class T>
ostream& operator<<(ostream& os, const vector<T>& v) {
    os << "[";
    for (size_t i = 0; i < v.size(); ++i) os << (i ? "," : "") << v[i];
    return os << "]";
}

}  // namespace std
