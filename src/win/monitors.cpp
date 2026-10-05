#include "win/monitors.h"

#include "win/util.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace brocompositor::win {

MonitorRegistry::MonitorRegistry() = default;

MonitorRegistry::~MonitorRegistry() {
    stop();
}

bool MonitorRegistry::start(std::function<void()> wake, std::string* error) {
    brodisplays::DisplayServiceConfig config;
    config.enable_events = true;
    config.enable_gamma = false;
    std::string err;
    auto service = brodisplays::DisplayService::create(config, &err);
    if (!service) {
        if (error) *error = "brodisplays: " + err;
        return false;
    }
    service->events().set_wake(std::move(wake));
    auto snap = service->snapshot();
    std::lock_guard<std::mutex> lock(mutex_);
    displays_ = std::move(service);
    topology_ = std::move(snap);
    return true;
}

void MonitorRegistry::stop() {
    std::unique_ptr<brodisplays::DisplayService> service;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        service = std::move(displays_);
    }
    if (!service) return;
    service->events().set_wake(nullptr);
    service.reset();  // joins the watcher: no wake runs after this
}

bool MonitorRegistry::take_changes() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!displays_) return false;
    bool changed = false;
    for (auto& ev : displays_->events().drain()) {
        if (auto* c = std::get_if<brodisplays::DisplaysChanged>(&ev)) {
            topology_ = std::move(c->snapshot);
            changed = true;
        }
    }
    return changed;
}

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
    std::map<std::wstring, std::pair<HMONITOR, RECT>> by_device;
    for (HMONITOR h : handles) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(h, &mi)) by_device[mi.szDevice] = {h, mi.rcWork};
    }

    std::vector<MonitorSnapshot> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : topology_.displays) {
        // A mirror shows another display's desktop and a sleeping or
        // disabled one shows none: neither is a place for windows.
        if (!d.is_active || !d.mirror_of.empty() || d.device_name.empty()) continue;
        const std::wstring device = to_wide(d.device_name);
        auto it = by_device.find(device);
        if (it == by_device.end()) continue;  // gone since brodisplays looked; its change event follows
        MonitorSnapshot s;
        s.id = id_for_name(device);
        s.name = d.device_name;
        s.bounds = Rect{d.geometry.x, d.geometry.y, int32_t(d.geometry.width), int32_t(d.geometry.height)};
        s.work_area = to_rect(it->second.second);
        s.dpi = d.scale.dpi > 0 ? uint32_t(d.scale.dpi) : 96u;
        s.primary = d.is_primary;
        s.native = uint64_t(reinterpret_cast<uintptr_t>(it->second.first));
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
