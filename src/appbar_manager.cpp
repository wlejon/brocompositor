#include "appbar_manager.h"
#include <algorithm>

namespace brocompositor {

AppBarManager::AppBarManager() {
    // Default primary monitor bounds (1080p fallback until OS reports)
    monitor_bounds_[PrimaryMonitorId] = Rect{0, 0, 1920, 1080};
    recompute_work_area_locked(PrimaryMonitorId);
}

void AppBarManager::set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) {
    std::lock_guard<std::mutex> lock(mutex_);
    monitor_bounds_[monitor_id] = bounds;
    recompute_work_area_locked(monitor_id);
}

bool AppBarManager::register_appbar(const AppBarInfo& bar) {
    std::lock_guard<std::mutex> lock(mutex_);
    AppBarId id = bar.id != InvalidAppBarId ? bar.id : next_id_++;

    AppBarInfo info = bar;
    info.id = id;

    // Calculate bounds if not pre-populated
    if (info.bounds.empty()) {
        auto mon_it = monitor_bounds_.find(info.monitor_id);
        Rect mon_rect = (mon_it != monitor_bounds_.end()) ? mon_it->second : Rect{0, 0, 1920, 1080};

        switch (info.edge) {
            case Edge::Top:
                info.bounds = Rect{mon_rect.x, mon_rect.y, mon_rect.width, info.thickness};
                break;
            case Edge::Bottom:
                info.bounds = Rect{mon_rect.x, mon_rect.bottom() - info.thickness, mon_rect.width, info.thickness};
                break;
            case Edge::Left:
                info.bounds = Rect{mon_rect.x, mon_rect.y, info.thickness, mon_rect.height};
                break;
            case Edge::Right:
                info.bounds = Rect{mon_rect.right() - info.thickness, mon_rect.y, info.thickness, mon_rect.height};
                break;
        }
    }

    appbars_[id] = info;
    recompute_work_area_locked(info.monitor_id);
    return true;
}

bool AppBarManager::unregister_appbar(AppBarId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = appbars_.find(id);
    if (it == appbars_.end()) {
        return false;
    }

    MonitorId mon_id = it->second.monitor_id;
    appbars_.erase(it);
    recompute_work_area_locked(mon_id);
    return true;
}

std::vector<AppBarInfo> AppBarManager::get_appbars(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppBarInfo> res;
    for (const auto& pair : appbars_) {
        if (pair.second.monitor_id == monitor_id) {
            res.push_back(pair.second);
        }
    }
    return res;
}

WorkArea AppBarManager::get_work_area(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = work_areas_.find(monitor_id);
    if (it != work_areas_.end()) {
        return it->second;
    }

    auto mon_it = monitor_bounds_.find(monitor_id);
    Rect total = (mon_it != monitor_bounds_.end()) ? mon_it->second : Rect{0, 0, 1920, 1080};
    return WorkArea{monitor_id, total, total, Margins{0, 0, 0, 0}};
}

Rect AppBarManager::compute_appbar_rect(MonitorId monitor_id, Edge edge, int32_t thickness) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto mon_it = monitor_bounds_.find(monitor_id);
    Rect mon = (mon_it != monitor_bounds_.end()) ? mon_it->second : Rect{0, 0, 1920, 1080};

    // Calculate existing edge thickness
    int32_t existing_offset = 0;
    for (const auto& pair : appbars_) {
        if (pair.second.monitor_id == monitor_id && pair.second.edge == edge) {
            existing_offset += pair.second.thickness;
        }
    }

    switch (edge) {
        case Edge::Top:
            return Rect{mon.x, mon.y + existing_offset, mon.width, thickness};
        case Edge::Bottom:
            return Rect{mon.x, mon.bottom() - existing_offset - thickness, mon.width, thickness};
        case Edge::Left:
            return Rect{mon.x + existing_offset, mon.y, thickness, mon.height};
        case Edge::Right:
            return Rect{mon.right() - existing_offset - thickness, mon.y, thickness, mon.height};
        default:
            return {};
    }
}

void AppBarManager::recompute_work_area_locked(MonitorId monitor_id) {
    auto mon_it = monitor_bounds_.find(monitor_id);
    Rect total = (mon_it != monitor_bounds_.end()) ? mon_it->second : Rect{0, 0, 1920, 1080};

    Margins reserved{0, 0, 0, 0};

    for (const auto& pair : appbars_) {
        if (pair.second.monitor_id != monitor_id || pair.second.auto_hide) {
            continue;
        }

        switch (pair.second.edge) {
            case Edge::Top:
                reserved.top += pair.second.thickness;
                break;
            case Edge::Bottom:
                reserved.bottom += pair.second.thickness;
                break;
            case Edge::Left:
                reserved.left += pair.second.thickness;
                break;
            case Edge::Right:
                reserved.right += pair.second.thickness;
                break;
        }
    }

    WorkArea wa;
    wa.monitor_id = monitor_id;
    wa.total_monitor_rect = total;
    wa.reserved_margins = reserved;
    wa.available_work_area = total.inset(reserved);

    work_areas_[monitor_id] = wa;
}

} // namespace brocompositor
