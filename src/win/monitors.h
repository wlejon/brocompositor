#pragma once

#include "brocompositor/events.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::win {

// Maps monitors to stable MonitorIds (keyed by device name, so an id
// survives HMONITOR churn across display reconfiguration) and snapshots them.
// Thread-safe; callers must be in a per-monitor-v2 DPI context.
class MonitorRegistry {
public:
    std::vector<MonitorSnapshot> enumerate();
    MonitorId id_for(HMONITOR monitor);
    std::optional<MonitorSnapshot> find(MonitorId id);
    std::vector<MonitorSnapshot> last() const;

private:
    MonitorId id_for_name(const std::wstring& device);

    mutable std::mutex mutex_;
    std::map<std::wstring, MonitorId> ids_;
    MonitorId next_ = 1;
    std::vector<MonitorSnapshot> last_;
};

}  // namespace brocompositor::win
