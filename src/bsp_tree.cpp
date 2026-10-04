#include "bsp_tree.h"
#include <algorithm>
#include <queue>

namespace brocompositor {

bool BspTree::insert(WindowId new_win, WindowId target_win, SplitDirection preferred_dir) {
    if (new_win == InvalidWindowId || contains(new_win)) {
        return false;
    }

    if (!root_) {
        root_ = std::make_unique<BspNode>(new_win);
        return true;
    }

    BspNode* target = nullptr;
    if (target_win != InvalidWindowId) {
        target = find_leaf(root_.get(), target_win);
    }
    if (!target) {
        target = find_deepest_leaf(root_.get());
    }
    if (!target) {
        return false;
    }

    // Determine split direction
    SplitDirection dir = preferred_dir;
    if (dir == SplitDirection::Auto) {
        // Toggle based on depth or default to vertical
        int depth = 0;
        BspNode* p = target->parent;
        while (p) {
            depth++;
            p = p->parent;
        }
        dir = (depth % 2 == 0) ? SplitDirection::Vertical : SplitDirection::Horizontal;
    }

    WindowId old_win = target->window_id;

    auto old_leaf = std::make_unique<BspNode>(old_win);
    auto new_leaf = std::make_unique<BspNode>(new_win);

    old_leaf->parent = target;
    new_leaf->parent = target;

    target->is_leaf = false;
    target->window_id = InvalidWindowId;
    target->split = dir;
    target->ratio = 0.5f;
    target->left = std::move(old_leaf);
    target->right = std::move(new_leaf);

    return true;
}

bool BspTree::remove(WindowId win) {
    if (win == InvalidWindowId || !root_) {
        return false;
    }

    BspNode* leaf = find_leaf(root_.get(), win);
    if (!leaf) {
        return false;
    }

    // If it's the root itself
    if (leaf == root_.get()) {
        root_.reset();
        return true;
    }

    BspNode* parent = leaf->parent;
    BspNode* grandparent = parent->parent;

    // Find the sibling
    std::unique_ptr<BspNode> sibling;
    if (parent->left.get() == leaf) {
        sibling = std::move(parent->right);
    } else {
        sibling = std::move(parent->left);
    }

    sibling->parent = grandparent;

    if (!grandparent) {
        // Parent was root
        root_ = std::move(sibling);
    } else {
        if (grandparent->left.get() == parent) {
            grandparent->left = std::move(sibling);
        } else {
            grandparent->right = std::move(sibling);
        }
    }

    return true;
}

bool BspTree::contains(WindowId win) const {
    if (win == InvalidWindowId || !root_) return false;
    return find_leaf(root_.get(), win) != nullptr;
}

std::vector<WindowId> BspTree::get_windows() const {
    std::vector<WindowId> wins;
    if (root_) {
        collect_windows(root_.get(), wins);
    }
    return wins;
}

void BspTree::collect_windows(BspNode* node, std::vector<WindowId>& out) const {
    if (!node) return;
    if (node->is_leaf) {
        if (node->window_id != InvalidWindowId) {
            out.push_back(node->window_id);
        }
    } else {
        collect_windows(node->left.get(), out);
        collect_windows(node->right.get(), out);
    }
}

BspNode* BspTree::find_leaf(BspNode* node, WindowId win) const {
    if (!node) return nullptr;
    if (node->is_leaf) {
        return (node->window_id == win) ? node : nullptr;
    }
    BspNode* l = find_leaf(node->left.get(), win);
    if (l) return l;
    return find_leaf(node->right.get(), win);
}

BspNode* BspTree::find_deepest_leaf(BspNode* node) const {
    if (!node) return nullptr;
    if (node->is_leaf) return node;

    // BFS to find deepest
    std::queue<BspNode*> q;
    q.push(node);
    BspNode* last_leaf = nullptr;

    while (!q.empty()) {
        BspNode* curr = q.front();
        q.pop();

        if (curr->is_leaf) {
            last_leaf = curr;
        } else {
            if (curr->left) q.push(curr->left.get());
            if (curr->right) q.push(curr->right.get());
        }
    }
    return last_leaf;
}

void BspTree::compute_rects(
    const Rect& work_area,
    int32_t inner_gap,
    std::unordered_map<WindowId, Rect>& out_rects
) const {
    if (!root_ || work_area.empty()) {
        return;
    }
    compute_node_rects(root_.get(), work_area, inner_gap, out_rects);
}

void BspTree::compute_node_rects(
    const BspNode* node,
    const Rect& node_rect,
    int32_t inner_gap,
    std::unordered_map<WindowId, Rect>& out_rects
) const {
    if (!node || node_rect.empty()) return;

    if (node->is_leaf) {
        if (node->window_id != InvalidWindowId) {
            out_rects[node->window_id] = node_rect;
        }
        return;
    }

    int32_t gap = std::max(0, inner_gap);

    if (node->split == SplitDirection::Vertical) {
        // Split along X (left / right)
        int32_t available_w = std::max(0, node_rect.width - gap);
        int32_t left_w = static_cast<int32_t>(available_w * node->ratio);
        int32_t right_w = available_w - left_w;

        Rect left_rect{node_rect.x, node_rect.y, left_w, node_rect.height};
        Rect right_rect{node_rect.x + left_w + gap, node_rect.y, right_w, node_rect.height};

        compute_node_rects(node->left.get(), left_rect, gap, out_rects);
        compute_node_rects(node->right.get(), right_rect, gap, out_rects);
    } else {
        // Split along Y (top / bottom)
        int32_t available_h = std::max(0, node_rect.height - gap);
        int32_t top_h = static_cast<int32_t>(available_h * node->ratio);
        int32_t bottom_h = available_h - top_h;

        Rect top_rect{node_rect.x, node_rect.y, node_rect.width, top_h};
        Rect bottom_rect{node_rect.x, node_rect.y + top_h + gap, node_rect.width, bottom_h};

        compute_node_rects(node->left.get(), top_rect, gap, out_rects);
        compute_node_rects(node->right.get(), bottom_rect, gap, out_rects);
    }
}

} // namespace brocompositor
