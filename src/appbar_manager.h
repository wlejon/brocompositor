#pragma once

#include "brocompositor/appbar.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace brocompositor {

class AppBarManager : public IAppBarManager {
public:
    AppBarManager();
    ~AppBarManager() override = default;

    bool register_appbar(const AppBarInfo& bar) override;
    bool unregister_appbar(AppBarId id) override;
    std::vector<AppBarInfo> get_appbars(MonitorId monitor_id) const override;
    WorkArea get_work_area(MonitorId monitor_id) const override;
    void set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) override;

    // Helper to calculate exact placement for a new/resized appbar
    Rect compute_appbar_rect(MonitorId monitor_id, Edge edge, int32_t thickness) const;

private:
    void recompute_work_area_locked(MonitorId monitor_id);

    mutable std::mutex mutex_;
    AppBarId next_id_ = 1;

    std::unordered_map<MonitorId, Rect> monitor_bounds_;
    std::unordered_map<MonitorId, WorkArea> work_areas_;
    std::unordered_map<AppBarId, AppBarInfo> appbars_;
};

} // namespace brocompositor
