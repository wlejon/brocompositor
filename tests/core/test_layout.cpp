#include "brocompositor/layout.h"

#include "check.h"
#include "printers.h"

using namespace brocompositor;

namespace {

const Rect kArea{0, 0, 1920, 1080};

LayoutConfig no_gaps() {
    LayoutConfig c;
    c.gap_inner = 0;
    c.gap_outer = 0;
    return c;
}

bool pairwise_disjoint(const LayoutResult& r) {
    for (size_t i = 0; i < r.placements.size(); ++i)
        for (size_t j = i + 1; j < r.placements.size(); ++j)
            if (r.placements[i].rect.intersects(r.placements[j].rect)) return false;
    return true;
}

int64_t total_area(const LayoutResult& r) {
    int64_t a = 0;
    for (const auto& p : r.placements) a += p.rect.area();
    return a;
}

void master_stack() {
    auto r1 = compute_layout(LayoutMode::MasterStack, kArea, {101}, no_gaps());
    CHECK_EQ(r1.placements.size(), size_t(1));
    CHECK_EQ(*r1.find(101), kArea);

    auto r2 = compute_layout(LayoutMode::MasterStack, kArea, {101, 102}, no_gaps());
    CHECK_EQ(*r2.find(101), (Rect{0, 0, 960, 1080}));
    CHECK_EQ(*r2.find(102), (Rect{960, 0, 960, 1080}));

    LayoutConfig g;
    g.gap_inner = 10;
    g.gap_outer = 10;
    auto r3 = compute_layout(LayoutMode::MasterStack, kArea, {1, 2, 3}, g);
    Rect m = *r3.find(1), s1 = *r3.find(2), s2 = *r3.find(3);
    CHECK(pairwise_disjoint(r3));
    CHECK_EQ(m.x, 10);
    CHECK_EQ(s1.x, m.right() + 10);
    CHECK_EQ(s2.y, s1.bottom() + 10);
    CHECK_EQ(s2.bottom(), 1070);
    CHECK_EQ(s1.right(), 1910);

    LayoutConfig two = no_gaps();
    two.master_count = 2;
    auto r4 = compute_layout(LayoutMode::MasterStack, kArea, {1, 2, 3}, two);
    CHECK_EQ(r4.find(1)->x, 0);
    CHECK_EQ(r4.find(2)->x, 0);
    CHECK_EQ(r4.find(3)->x, 960);
    CHECK_EQ(r4.find(3)->height, 1080);
}

void columns_and_grid() {
    auto c = compute_layout(LayoutMode::Columns, kArea, {1, 2, 3}, no_gaps());
    CHECK_EQ(*c.find(1), (Rect{0, 0, 640, 1080}));
    CHECK_EQ(*c.find(3), (Rect{1280, 0, 640, 1080}));

    auto g = compute_layout(LayoutMode::Grid, kArea, {1, 2, 3, 4}, no_gaps());
    CHECK_EQ(*g.find(1), (Rect{0, 0, 960, 540}));
    CHECK_EQ(*g.find(4), (Rect{960, 540, 960, 540}));

    // 5 windows: 3 columns, the last row has 2 that share the full width.
    auto g5 = compute_layout(LayoutMode::Grid, kArea, {1, 2, 3, 4, 5}, no_gaps());
    CHECK_EQ(g5.find(4)->width, 960);
    CHECK(pairwise_disjoint(g5));
    CHECK_EQ(total_area(g5), kArea.area());
}

void bsp() {
    auto r = compute_layout(LayoutMode::BSP, kArea, {10, 20, 30}, no_gaps());
    CHECK_EQ(*r.find(10), (Rect{0, 0, 960, 1080}));
    CHECK_EQ(*r.find(20), (Rect{960, 0, 960, 540}));
    CHECK_EQ(*r.find(30), (Rect{960, 540, 960, 540}));
    for (int n = 1; n <= 9; ++n) {
        std::vector<WindowId> ids;
        for (int i = 0; i < n; ++i) ids.push_back(WindowId(i + 1));
        auto rn = compute_layout(LayoutMode::BSP, kArea, ids, no_gaps());
        CHECK_EQ(rn.placements.size(), size_t(n));
        CHECK(pairwise_disjoint(rn));
        CHECK_EQ(total_area(rn), kArea.area());
    }
}

void floating_and_degenerate() {
    CHECK(compute_layout(LayoutMode::Floating, kArea, {1, 2}, no_gaps()).placements.empty());
    CHECK(compute_layout(LayoutMode::BSP, kArea, {}, no_gaps()).placements.empty());
    CHECK(compute_layout(LayoutMode::BSP, Rect{0, 0, 0, 0}, {1}, no_gaps()).placements.empty());
    // Non-origin areas (second monitor, work area under a top bar).
    Rect off{1920, 40, 1280, 984};
    auto r = compute_layout(LayoutMode::Columns, off, {1, 2}, no_gaps());
    CHECK_EQ(*r.find(1), (Rect{1920, 40, 640, 984}));
    CHECK_EQ(*r.find(2), (Rect{2560, 40, 640, 984}));
}

void snapping() {
    CHECK_EQ(snap_rect(Rect{8, 100, 400, 300}, kArea, {}, 16).x, 0);
    CHECK_EQ(snap_rect(Rect{1920 - 408, 100, 400, 300}, kArea, {}, 16).right(), 1920);
    Rect s = snap_rect(Rect{408, 105, 200, 200}, kArea, {Rect{100, 100, 300, 300}}, 16);
    CHECK_EQ(s.x, 400);
    CHECK_EQ(s.y, 100);
    CHECK_EQ(snap_rect(Rect{500, 500, 100, 100}, kArea, {}, 16), (Rect{500, 500, 100, 100}));
}

}  // namespace

int main() {
    master_stack();
    columns_and_grid();
    bsp();
    floating_and_degenerate();
    snapping();
    return bctest::finish("test_layout");
}
