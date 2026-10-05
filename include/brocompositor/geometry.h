// Integer geometry shared by every brocompositor layer. All coordinates are
// physical pixels in the desktop's global coordinate space (on Windows, the
// per-monitor-DPI-aware virtual screen; on Linux, the compositor's layout
// space). The exception is macOS, whose global display space has no single
// pixel grid across mixed-scale displays: there coordinates are Quartz
// global display points (origin at the top-left of the primary display, y
// down), and MonitorSnapshot::dpi / 96 is the display's points-to-pixels
// scale. Nothing here depends on a platform.
#pragma once

#include <algorithm>
#include <cstdint>

namespace brocompositor {

enum class Edge : uint32_t { Left = 0, Top = 1, Right = 2, Bottom = 3 };

enum class Direction : uint32_t { Left = 0, Up = 1, Right = 2, Down = 3 };

struct Point {
    int32_t x = 0;
    int32_t y = 0;
    constexpr bool operator==(const Point&) const = default;
};

struct Size {
    int32_t width = 0;
    int32_t height = 0;
    constexpr bool operator==(const Size&) const = default;
    constexpr bool empty() const { return width <= 0 || height <= 0; }
};

struct Margins {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    constexpr bool operator==(const Margins&) const = default;
    constexpr int32_t horizontal() const { return left + right; }
    constexpr int32_t vertical() const { return top + bottom; }
};

struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    constexpr bool operator==(const Rect&) const = default;

    constexpr int32_t left() const { return x; }
    constexpr int32_t top() const { return y; }
    constexpr int32_t right() const { return x + width; }
    constexpr int32_t bottom() const { return y + height; }
    constexpr Size size() const { return {width, height}; }
    constexpr Point center() const { return {x + width / 2, y + height / 2}; }
    constexpr bool empty() const { return width <= 0 || height <= 0; }
    constexpr int64_t area() const { return empty() ? 0 : int64_t(width) * height; }

    constexpr bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }
    constexpr bool contains(const Rect& r) const {
        return r.x >= x && r.right() <= right() && r.y >= y && r.bottom() <= bottom();
    }
    constexpr bool intersects(const Rect& r) const {
        return !empty() && !r.empty() && x < r.right() && right() > r.x && y < r.bottom() &&
               bottom() > r.y;
    }
    constexpr Rect intersected(const Rect& r) const {
        int32_t l = std::max(x, r.x), t = std::max(y, r.y);
        int32_t rr = std::min(right(), r.right()), b = std::min(bottom(), r.bottom());
        if (rr <= l || b <= t) return Rect{0, 0, 0, 0};
        return Rect{l, t, rr - l, b - t};
    }
    constexpr Rect united(const Rect& r) const {
        if (empty()) return r;
        if (r.empty()) return *this;
        int32_t l = std::min(x, r.x), t = std::min(y, r.y);
        int32_t rr = std::max(right(), r.right()), b = std::max(bottom(), r.bottom());
        return Rect{l, t, rr - l, b - t};
    }
    constexpr Rect inset(const Margins& m) const {
        return Rect{x + m.left, y + m.top, std::max(0, width - m.horizontal()),
                    std::max(0, height - m.vertical())};
    }
    constexpr Rect outset(const Margins& m) const {
        return Rect{x - m.left, y - m.top, width + m.horizontal(), height + m.vertical()};
    }
};

}  // namespace brocompositor
