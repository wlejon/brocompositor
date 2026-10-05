// The displays the macOS shell reports. WindowServer owns them; which ones
// exist, where (Quartz points), at what scale, which holds the menu bar, and
// when that changes comes from brodisplays (its watcher: the CoreGraphics
// reconfiguration callback plus a topology poll, so changes arrive in a host
// that runs no main run loop). The shell adds what only it cares about:
// NSScreen's menu-bar / Dock insets of the visible frame, and the localized
// name. Thread-safe.
#pragma once

#include "mac/system.h"

#include <brodisplays/display_service.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace brocompositor::mac {

class DisplayTopology {
public:
    DisplayTopology();
    ~DisplayTopology();

    // Starts watching; `wake` runs on a brodisplays thread whenever a
    // topology change is queued.
    bool start(std::function<void()> wake, std::string* error);
    // Stops watching; no wake runs once this returns.
    void stop();

    // The active, non-mirroring displays as of the latest change brodisplays
    // reported. Empty while every display is asleep (the window server then
    // reports no geometry worth trusting).
    std::vector<sys::Screen> screens();

private:
    std::unique_ptr<brodisplays::DisplayService> service_;
    std::mutex mutex_;
    brodisplays::DisplaysSnapshot topology_;
};

}  // namespace brocompositor::mac
