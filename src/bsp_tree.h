#pragma once

#include "brocompositor/types.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace brocompositor {

struct BspNode {
    WindowId window_id = InvalidWindowId;
    bool is_leaf = true;
    SplitDirection split = SplitDirection::Vertical;
    float ratio = 0.5f;

    std::unique_ptr<BspNode> left;
    std::unique_ptr<BspNode> right;
    BspNode* parent = nullptr;

    explicit BspNode(WindowId id) : window_id(id), is_leaf(true) {}
    BspNode(SplitDirection dir, float r) : is_leaf(false), split(dir), ratio(r) {}
};

class BspTree {
public:
    BspTree() = default;
    ~BspTree() = default;

    // Non-copyable, movable
    BspTree(const BspTree&) = delete;
    BspTree& operator=(const BspTree&) = delete;
    BspTree(BspTree&&) noexcept = default;
    BspTree& operator=(BspTree&&) noexcept = default;

    bool empty() const { return root_ == nullptr; }
    void clear() { root_.reset(); }

    bool insert(WindowId new_win, WindowId target_win = InvalidWindowId, SplitDirection preferred_dir = SplitDirection::Auto);
    bool remove(WindowId win);
    bool contains(WindowId win) const;

    std::vector<WindowId> get_windows() const;

    void compute_rects(
        const Rect& work_area,
        int32_t inner_gap,
        std::unordered_map<WindowId, Rect>& out_rects
    ) const;

private:
    BspNode* find_leaf(BspNode* node, WindowId win) const;
    BspNode* find_deepest_leaf(BspNode* node) const;
    void collect_windows(BspNode* node, std::vector<WindowId>& out) const;
    void compute_node_rects(
        const BspNode* node,
        const Rect& node_rect,
        int32_t inner_gap,
        std::unordered_map<WindowId, Rect>& out_rects
    ) const;

    std::unique_ptr<BspNode> root_;
};

} // namespace brocompositor
