#include "brocompositor/layout.h"

#include <cmath>
#include <cstdlib>

namespace brocompositor {

const char* to_string(LayoutMode mode) {
    switch (mode) {
        case LayoutMode::Floating: return "floating";
        case LayoutMode::BSP: return "bsp";
        case LayoutMode::MasterStack: return "master-stack";
        case LayoutMode::Columns: return "columns";
        case LayoutMode::Grid: return "grid";
    }
    return "unknown";
}

namespace {

// Splits `total` into `count` sizes separated by `gap`, remainder to the front.
std::vector<int32_t> partition(int32_t total, int32_t count, int32_t gap) {
    std::vector<int32_t> sizes;
    if (count <= 0) return sizes;
    int32_t avail = std::max(0, total - (count - 1) * gap);
    sizes.assign(size_t(count), avail / count);
    for (int32_t i = 0; i < avail % count; ++i) sizes[size_t(i)] += 1;
    return sizes;
}

void stack(LayoutResult& out, const std::vector<WindowId>& wins, size_t first, size_t count,
           int32_t x, int32_t y, int32_t w, int32_t h, int32_t gap) {
    auto heights = partition(h, int32_t(count), gap);
    for (size_t i = 0; i < count; ++i) {
        out.placements.push_back({wins[first + i], Rect{x, y, w, heights[i]}});
        y += heights[i] + gap;
    }
}

void master_stack(LayoutResult& out, const Rect& a, const std::vector<WindowId>& wins,
                  const LayoutConfig& c, int32_t gap) {
    size_t n = wins.size();
    if (n == 1) {
        out.placements.push_back({wins[0], a});
        return;
    }
    size_t masters = size_t(std::clamp<int32_t>(c.master_count, 1, int32_t(n) - 1));
    float ratio = std::clamp(c.master_ratio, 0.1f, 0.9f);
    int32_t avail = std::max(0, a.width - gap);
    int32_t mw = int32_t(float(avail) * ratio);
    stack(out, wins, 0, masters, a.x, a.y, mw, a.height, gap);
    stack(out, wins, masters, n - masters, a.x + mw + gap, a.y, avail - mw, a.height, gap);
}

void columns(LayoutResult& out, const Rect& a, const std::vector<WindowId>& wins, int32_t gap) {
    auto widths = partition(a.width, int32_t(wins.size()), gap);
    int32_t x = a.x;
    for (size_t i = 0; i < wins.size(); ++i) {
        out.placements.push_back({wins[i], Rect{x, a.y, widths[i], a.height}});
        x += widths[i] + gap;
    }
}

void grid(LayoutResult& out, const Rect& a, const std::vector<WindowId>& wins, int32_t gap) {
    int32_t total = int32_t(wins.size());
    int32_t cols = std::max(1, int32_t(std::ceil(std::sqrt(double(total)))));
    int32_t rows = (total + cols - 1) / cols;
    auto heights = partition(a.height, rows, gap);
    int32_t idx = 0, y = a.y;
    for (int32_t r = 0; r < rows; ++r) {
        int32_t in_row = std::min(cols, total - idx);
        auto widths = partition(a.width, in_row, gap);
        int32_t x = a.x;
        for (int32_t col = 0; col < in_row; ++col, ++idx) {
            out.placements.push_back({wins[size_t(idx)], Rect{x, y, widths[size_t(col)], heights[size_t(r)]}});
            x += widths[size_t(col)] + gap;
        }
        y += heights[size_t(r)] + gap;
    }
}

// Dwindle BSP: window i takes the first half of the remaining area, the rest
// recurse into the second half; the split axis alternates with depth.
void bsp(LayoutResult& out, Rect a, const std::vector<WindowId>& wins, const LayoutConfig& c,
         int32_t gap) {
    for (size_t i = 0; i < wins.size(); ++i) {
        if (i + 1 == wins.size()) {
            out.placements.push_back({wins[i], a});
            break;
        }
        bool vertical = c.bsp_split == SplitDirection::Vertical ||
                        (c.bsp_split == SplitDirection::Auto && i % 2 == 0);
        if (vertical) {
            int32_t avail = std::max(0, a.width - gap);
            int32_t first = avail / 2 + avail % 2;
            out.placements.push_back({wins[i], Rect{a.x, a.y, first, a.height}});
            a = Rect{a.x + first + gap, a.y, avail - first, a.height};
        } else {
            int32_t avail = std::max(0, a.height - gap);
            int32_t first = avail / 2 + avail % 2;
            out.placements.push_back({wins[i], Rect{a.x, a.y, a.width, first}});
            a = Rect{a.x, a.y + first + gap, a.width, avail - first};
        }
    }
}

}  // namespace

LayoutResult compute_layout(LayoutMode mode, const Rect& area, const std::vector<WindowId>& wins,
                            const LayoutConfig& c) {
    LayoutResult out;
    if (wins.empty() || area.empty() || mode == LayoutMode::Floating) return out;
    int32_t og = std::max(0, c.gap_outer);
    Rect a = area.inset(Margins{og, og, og, og});
    if (a.empty()) a = area;
    int32_t gap = std::max(0, c.gap_inner);
    switch (mode) {
        case LayoutMode::MasterStack: master_stack(out, a, wins, c, gap); break;
        case LayoutMode::Columns: columns(out, a, wins, gap); break;
        case LayoutMode::Grid: grid(out, a, wins, gap); break;
        case LayoutMode::BSP: bsp(out, a, wins, c, gap); break;
        case LayoutMode::Floating: break;
    }
    return out;
}

Rect snap_rect(const Rect& rect, const Rect& area, const std::vector<Rect>& others, int32_t d) {
    if (d <= 0) return rect;
    Rect s = rect;
    auto near = [d](int32_t a, int32_t b) { return std::abs(a - b) <= d; };
    if (near(s.left(), area.left())) s.x = area.left();
    if (near(s.right(), area.right())) s.x = area.right() - s.width;
    if (near(s.top(), area.top())) s.y = area.top();
    if (near(s.bottom(), area.bottom())) s.y = area.bottom() - s.height;
    for (const Rect& o : others) {
        if (o == rect) continue;
        bool v_overlap = s.bottom() > o.top() && s.top() < o.bottom();
        if (v_overlap) {
            if (near(s.right(), o.left())) s.x = o.left() - s.width;
            else if (near(s.left(), o.right())) s.x = o.right();
            if (near(s.top(), o.top())) s.y = o.top();
            if (near(s.bottom(), o.bottom())) s.y = o.bottom() - s.height;
        }
        bool h_overlap = s.right() > o.left() && s.left() < o.right();
        if (h_overlap) {
            if (near(s.bottom(), o.top())) s.y = o.top() - s.height;
            else if (near(s.top(), o.bottom())) s.y = o.bottom();
            if (near(s.left(), o.left())) s.x = o.left();
            if (near(s.right(), o.right())) s.x = o.right() - s.width;
        }
    }
    return s;
}

}  // namespace brocompositor
