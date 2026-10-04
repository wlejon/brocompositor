#pragma once

#include "brocompositor/export.h"
#include <cstdint>
#include <algorithm>
#include <string>

namespace brocompositor {

using WindowId = uint64_t;
constexpr WindowId InvalidWindowId = 0;

using WorkspaceId = uint32_t;
constexpr WorkspaceId InvalidWorkspaceId = 0;

using MonitorId = uint32_t;
constexpr MonitorId PrimaryMonitorId = 1;

using AppBarId = uint64_t;
constexpr AppBarId InvalidAppBarId = 0;

enum class Edge : uint32_t {
    Left = 0,
    Top = 1,
    Right = 2,
    Bottom = 3
};

inline const char* edge_to_string(Edge edge) {
    switch (edge) {
        case Edge::Left:   return "Left";
        case Edge::Top:    return "Top";
        case Edge::Right:  return "Right";
        case Edge::Bottom: return "Bottom";
        default:           return "Unknown";
    }
}

enum class WindowState : uint32_t {
    None        = 0,
    Minimized   = 1 << 0,
    Maximized   = 1 << 1,
    Fullscreen  = 1 << 2,
    Tiled       = 1 << 3,
    Floating    = 1 << 4,
    Focused     = 1 << 5,
    Hidden      = 1 << 6,
    Modal       = 1 << 7,
    Pinned      = 1 << 8
};

inline constexpr WindowState operator|(WindowState a, WindowState b) {
    return static_cast<WindowState>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr WindowState operator&(WindowState a, WindowState b) {
    return static_cast<WindowState>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr WindowState operator^(WindowState a, WindowState b) {
    return static_cast<WindowState>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b));
}

inline constexpr WindowState operator~(WindowState a) {
    return static_cast<WindowState>(~static_cast<uint32_t>(a));
}

inline WindowState& operator|=(WindowState& a, WindowState b) {
    a = a | b;
    return a;
}

inline WindowState& operator&=(WindowState& a, WindowState b) {
    a = a & b;
    return a;
}

inline WindowState& operator^=(WindowState& a, WindowState b) {
    a = a ^ b;
    return a;
}

inline constexpr bool has_state(WindowState value, WindowState flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
}

enum class LayoutMode : uint32_t {
    Floating    = 0,
    BSP         = 1,
    MasterStack = 2,
    Columns     = 3,
    Grid        = 4
};

inline const char* layout_mode_to_string(LayoutMode mode) {
    switch (mode) {
        case LayoutMode::Floating:    return "Floating";
        case LayoutMode::BSP:         return "BSP";
        case LayoutMode::MasterStack: return "MasterStack";
        case LayoutMode::Columns:     return "Columns";
        case LayoutMode::Grid:        return "Grid";
        default:                      return "Unknown";
    }
}

enum class SplitDirection : uint32_t {
    Horizontal = 0,
    Vertical   = 1,
    Auto       = 2
};

struct Point {
    int32_t x = 0;
    int32_t y = 0;

    constexpr bool operator==(const Point& o) const = default;
    constexpr Point operator+(const Point& o) const { return {x + o.x, y + o.y}; }
    constexpr Point operator-(const Point& o) const { return {x - o.x, y - o.y}; }
};

struct Size {
    int32_t width = 0;
    int32_t height = 0;

    constexpr bool operator==(const Size& o) const = default;
    constexpr bool empty() const { return width <= 0 || height <= 0; }
    constexpr int64_t area() const { return static_cast<int64_t>(width) * height; }
};

struct Margins {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    constexpr bool operator==(const Margins& o) const = default;
    constexpr int32_t horizontal() const { return left + right; }
    constexpr int32_t vertical() const { return top + bottom; }
};

struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    constexpr bool operator==(const Rect& o) const = default;

    constexpr int32_t left() const { return x; }
    constexpr int32_t top() const { return y; }
    constexpr int32_t right() const { return x + width; }
    constexpr int32_t bottom() const { return y + height; }

    constexpr Point topleft() const { return {x, y}; }
    constexpr Point bottomright() const { return {x + width, y + height}; }
    constexpr Size size() const { return {width, height}; }

    constexpr bool empty() const { return width <= 0 || height <= 0; }
    constexpr int64_t area() const { return empty() ? 0 : static_cast<int64_t>(width) * height; }

    constexpr bool contains(const Point& pt) const {
        return pt.x >= x && pt.x < (x + width) && pt.y >= y && pt.y < (y + height);
    }

    constexpr bool contains(const Rect& r) const {
        return r.x >= x && r.right() <= right() && r.y >= y && r.bottom() <= bottom();
    }

    constexpr bool intersects(const Rect& r) const {
        return !empty() && !r.empty() &&
               x < r.right() && right() > r.x &&
               y < r.bottom() && bottom() > r.y;
    }

    constexpr Rect intersected(const Rect& r) const {
        int32_t nx = std::max(x, r.x);
        int32_t ny = std::max(y, r.y);
        int32_t nr = std::min(right(), r.right());
        int32_t nb = std::min(bottom(), r.bottom());
        if (nr <= nx || nb <= ny) {
            return {0, 0, 0, 0};
        }
        return {nx, ny, nr - nx, nb - ny};
    }

    constexpr Rect united(const Rect& r) const {
        if (empty()) return r;
        if (r.empty()) return *this;
        int32_t nx = std::min(x, r.x);
        int32_t ny = std::min(y, r.y);
        int32_t nr = std::max(right(), r.right());
        int32_t nb = std::max(bottom(), r.bottom());
        return {nx, ny, nr - nx, nb - ny};
    }

    constexpr Rect inset(const Margins& m) const {
        int32_t nw = width - m.horizontal();
        int32_t nh = height - m.vertical();
        if (nw < 0) nw = 0;
        if (nh < 0) nh = 0;
        return {x + m.left, y + m.top, nw, nh};
    }

    constexpr Rect outset(const Margins& m) const {
        return {x - m.left, y - m.top, width + m.horizontal(), height + m.vertical()};
    }
};

} // namespace brocompositor
