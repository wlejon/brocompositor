#pragma once

#include "brocompositor/events.h"

#include <brodisplays/display_service.h>

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::win {

// The monitors the shell reports. Windows owns the displays, and which ones
// exist, where, at what DPI, which is primary, and when that changes comes
// from brodisplays (its watcher sees WM_DISPLAYCHANGE and the rest); the
// registry adds what only the shell cares about: the work area (rcWork,
// which appbars and the taskbar shape) and the HMONITOR, joined on the GDI
// device name. Ids are minted per device name, so an id survives HMONITOR
// churn across reconfiguration.
//
// Thread-safe; callers of enumerate() / id_for() must be in a per-monitor-v2
// DPI context.
class MonitorRegistry {
public:
    MonitorRegistry();
    ~MonitorRegistry();

    // Starts watching the displays; `wake` runs (on a brodisplays thread)
    // whenever a topology change is queued, and should get the shell thread
    // to call take_changes().
    bool start(std::function<void()> wake, std::string* error);
    // Stops watching; no wake runs once this returns.
    void stop();
    // Applies queued topology changes (shell thread); true when any arrived.
    bool take_changes();

    std::vector<MonitorSnapshot> enumerate();
    MonitorId id_for(HMONITOR monitor);
    std::optional<MonitorSnapshot> find(MonitorId id);
    std::vector<MonitorSnapshot> last() const;

private:
    MonitorId id_for_name(const std::wstring& device);

    std::unique_ptr<brodisplays::DisplayService> displays_;
    mutable std::mutex mutex_;
    brodisplays::DisplaysSnapshot topology_;
    std::map<std::wstring, MonitorId> ids_;
    MonitorId next_ = 1;
    std::vector<MonitorSnapshot> last_;
};

}  // namespace brocompositor::win
