#pragma once

#include "brocompositor/types.h"
#include <string>
#include <vector>

namespace brocompositor {

struct WorkspaceInfo {
    WorkspaceId id = 1;
    std::string name;
    MonitorId monitor_id = PrimaryMonitorId;
    WindowId active_window = InvalidWindowId;
    LayoutMode layout_mode = LayoutMode::MasterStack;
    std::vector<WindowId> windows;
};

} // namespace brocompositor
