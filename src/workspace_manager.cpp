#include "workspace_manager.h"
#include <algorithm>

namespace brocompositor {

WorkspaceManager::WorkspaceManager() {
    create_workspace("Default", PrimaryMonitorId);
}

WorkspaceId WorkspaceManager::create_workspace(const std::string& name, MonitorId monitor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    WorkspaceId id = next_workspace_id_++;

    WorkspaceInfo info;
    info.id = id;
    info.name = name.empty() ? ("Workspace " + std::to_string(id)) : name;
    info.monitor_id = monitor_id;
    info.active_window = InvalidWindowId;
    info.layout_mode = LayoutMode::MasterStack;

    workspaces_[id] = std::move(info);

    // If monitor has no active workspace, set this as active
    if (active_workspace_by_monitor_.find(monitor_id) == active_workspace_by_monitor_.end()) {
        active_workspace_by_monitor_[monitor_id] = id;
    }

    return id;
}

bool WorkspaceManager::remove_workspace(WorkspaceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) {
        return false;
    }

    MonitorId mon = it->second.monitor_id;
    std::vector<WindowId> wins = it->second.windows;

    workspaces_.erase(it);

    // If active workspace was removed, switch to another on the same monitor
    if (active_workspace_by_monitor_[mon] == id) {
        active_workspace_by_monitor_.erase(mon);
        for (const auto& pair : workspaces_) {
            if (pair.second.monitor_id == mon) {
                active_workspace_by_monitor_[mon] = pair.first;
                break;
            }
        }
    }

    WorkspaceId fallback = active_workspace_by_monitor_[mon];
    for (WindowId w : wins) {
        if (fallback != InvalidWorkspaceId) {
            workspaces_[fallback].windows.push_back(w);
            window_workspace_map_[w] = fallback;
        } else {
            window_workspace_map_.erase(w);
        }
    }

    return true;
}

bool WorkspaceManager::switch_workspace(WorkspaceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) {
        return false;
    }

    active_workspace_by_monitor_[it->second.monitor_id] = id;
    return true;
}

WorkspaceId WorkspaceManager::get_active_workspace(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_workspace_by_monitor_.find(monitor_id);
    return it != active_workspace_by_monitor_.end() ? it->second : InvalidWorkspaceId;
}

const WorkspaceInfo* WorkspaceManager::get_workspace(WorkspaceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(id);
    return it != workspaces_.end() ? &it->second : nullptr;
}

WorkspaceInfo* WorkspaceManager::get_workspace_mut(WorkspaceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(id);
    return it != workspaces_.end() ? &it->second : nullptr;
}

std::vector<WorkspaceId> WorkspaceManager::get_workspaces_on_monitor(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WorkspaceId> res;
    for (const auto& pair : workspaces_) {
        if (pair.second.monitor_id == monitor_id) {
            res.push_back(pair.first);
        }
    }
    std::sort(res.begin(), res.end());
    return res;
}

bool WorkspaceManager::set_workspace_layout_mode(WorkspaceId id, LayoutMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(id);
    if (it == workspaces_.end()) return false;
    it->second.layout_mode = mode;
    return true;
}

bool WorkspaceManager::add_window(WindowId win, WorkspaceId workspace_id) {
    if (win == InvalidWindowId) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(workspace_id);
    if (it == workspaces_.end()) return false;

    // Remove from existing workspace if present
    auto existing_it = window_workspace_map_.find(win);
    if (existing_it != window_workspace_map_.end()) {
        WorkspaceId old_ws = existing_it->second;
        auto old_it = workspaces_.find(old_ws);
        if (old_it != workspaces_.end()) {
            auto& old_wins = old_it->second.windows;
            old_wins.erase(std::remove(old_wins.begin(), old_wins.end(), win), old_wins.end());
            if (old_it->second.active_window == win) {
                old_it->second.active_window = old_wins.empty() ? InvalidWindowId : old_wins.back();
            }
        }
    }

    it->second.windows.push_back(win);
    it->second.active_window = win;
    window_workspace_map_[win] = workspace_id;
    return true;
}

bool WorkspaceManager::remove_window(WindowId win) {
    if (win == InvalidWindowId) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = window_workspace_map_.find(win);
    if (it == window_workspace_map_.end()) return false;

    WorkspaceId ws_id = it->second;
    window_workspace_map_.erase(it);

    auto ws_it = workspaces_.find(ws_id);
    if (ws_it != workspaces_.end()) {
        auto& wins = ws_it->second.windows;
        wins.erase(std::remove(wins.begin(), wins.end(), win), wins.end());
        if (ws_it->second.active_window == win) {
            ws_it->second.active_window = wins.empty() ? InvalidWindowId : wins.back();
        }
    }

    return true;
}

bool WorkspaceManager::move_window_to_workspace(WindowId win, WorkspaceId target_workspace) {
    if (win == InvalidWindowId) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto target_it = workspaces_.find(target_workspace);
    if (target_it == workspaces_.end()) return false;

    auto existing_it = window_workspace_map_.find(win);
    if (existing_it != window_workspace_map_.end()) {
        WorkspaceId old_ws = existing_it->second;
        if (old_ws == target_workspace) return true; // Already there

        auto old_it = workspaces_.find(old_ws);
        if (old_it != workspaces_.end()) {
            auto& old_wins = old_it->second.windows;
            old_wins.erase(std::remove(old_wins.begin(), old_wins.end(), win), old_wins.end());
            if (old_it->second.active_window == win) {
                old_it->second.active_window = old_wins.empty() ? InvalidWindowId : old_wins.back();
            }
        }
    }

    target_it->second.windows.push_back(win);
    target_it->second.active_window = win;
    window_workspace_map_[win] = target_workspace;
    return true;
}

WorkspaceId WorkspaceManager::get_window_workspace(WindowId win) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = window_workspace_map_.find(win);
    return it != window_workspace_map_.end() ? it->second : InvalidWorkspaceId;
}

bool WorkspaceManager::set_active_window(WorkspaceId workspace_id, WindowId win) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(workspace_id);
    if (it == workspaces_.end()) return false;

    if (win != InvalidWindowId) {
        auto& wins = it->second.windows;
        if (std::find(wins.begin(), wins.end(), win) == wins.end()) {
            return false;
        }
    }
    it->second.active_window = win;
    return true;
}

WindowId WorkspaceManager::get_active_window(WorkspaceId workspace_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(workspace_id);
    return it != workspaces_.end() ? it->second.active_window : InvalidWindowId;
}

void WorkspaceManager::set_workspace_monitor(WorkspaceId workspace_id, MonitorId monitor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(workspace_id);
    if (it != workspaces_.end()) {
        it->second.monitor_id = monitor_id;
    }
}

std::vector<WindowId> WorkspaceManager::get_windows(WorkspaceId workspace_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workspaces_.find(workspace_id);
    if (it != workspaces_.end()) {
        return it->second.windows;
    }
    return {};
}

} // namespace brocompositor
