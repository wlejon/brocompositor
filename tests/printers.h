#pragma once

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

}  // namespace brocompositor

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
