#pragma once

#include "brocompositor/types.h"
#include <string>
#include <vector>

namespace brocompositor {

struct AppBarInfo {
    AppBarId id = InvalidAppBarId;
    std::string name;
    MonitorId monitor_id = PrimaryMonitorId;
    Edge edge = Edge::Top;
    int32_t thickness = 32;
    Rect bounds;
    bool auto_hide = false;
};

struct WorkArea {
    MonitorId monitor_id = PrimaryMonitorId;
    Rect total_monitor_rect;
    Rect available_work_area;
    Margins reserved_margins;
};

class IAppBarManager {
public:
    virtual ~IAppBarManager() = default;

    virtual bool register_appbar(const AppBarInfo& bar) = 0;
    virtual bool unregister_appbar(AppBarId id) = 0;
    virtual std::vector<AppBarInfo> get_appbars(MonitorId monitor_id) const = 0;
    virtual WorkArea get_work_area(MonitorId monitor_id) const = 0;
    virtual void set_monitor_bounds(MonitorId monitor_id, const Rect& bounds) = 0;
};

} // namespace brocompositor
