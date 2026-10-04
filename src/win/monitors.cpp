#include "win/monitors.h"

#include "win/util.h"

#include <shellscalingapi.h>

#include <algorithm>

namespace brocompositor::win {

MonitorId MonitorRegistry::id_for_name(const std::wstring& device) {
    auto it = ids_.find(device);
    if (it != ids_.end()) return it->second;
    MonitorId id = next_++;
    ids_.emplace(device, id);
    return id;
}

std::vector<MonitorSnapshot> MonitorRegistry::enumerate() {
    std::vector<HMONITOR> handles;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&handles));

    std::vector<MonitorSnapshot> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (HMONITOR h : handles) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, &mi)) continue;
        MonitorSnapshot s;
        s.id = id_for_name(mi.szDevice);
        s.name = to_utf8(mi.szDevice);
        s.bounds = to_rect(mi.rcMonitor);
        s.work_area = to_rect(mi.rcWork);
        s.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        s.native = uint64_t(reinterpret_cast<uintptr_t>(h));
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dx, &dy))) s.dpi = dx;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const MonitorSnapshot& a, const MonitorSnapshot& b) {
        return a.bounds.x != b.bounds.x ? a.bounds.x < b.bounds.x : a.bounds.y < b.bounds.y;
    });
    last_ = out;
    return out;
}

MonitorId MonitorRegistry::id_for(HMONITOR monitor) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) return kNoMonitor;
    std::lock_guard<std::mutex> lock(mutex_);
    return id_for_name(mi.szDevice);
}

std::optional<MonitorSnapshot> MonitorRegistry::find(MonitorId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& m : last_)
        if (m.id == id) return m;
    return std::nullopt;
}

std::vector<MonitorSnapshot> MonitorRegistry::last() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

}  // namespace brocompositor::win
