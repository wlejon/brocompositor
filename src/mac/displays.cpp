#include "mac/displays.h"

#include <cmath>
#include <cstdlib>
#include <utility>
#include <variant>

namespace brocompositor::mac {

DisplayTopology::DisplayTopology() = default;

DisplayTopology::~DisplayTopology() {
    stop();
}

bool DisplayTopology::start(std::function<void()> wake, std::string* error) {
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
    service_ = std::move(service);
    topology_ = std::move(snap);
    return true;
}

void DisplayTopology::stop() {
    std::unique_ptr<brodisplays::DisplayService> service;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        service = std::move(service_);
    }
    if (!service) return;
    service->events().set_wake(nullptr);
    service.reset();  // joins the watcher: no wake runs after this
}

std::vector<sys::Screen> DisplayTopology::screens() {
    std::vector<brodisplays::DisplayInfo> displays;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (service_) {
            for (auto& ev : service_->events().drain())
                if (auto* c = std::get_if<brodisplays::DisplaysChanged>(&ev)) topology_ = std::move(c->snapshot);
        }
        displays = topology_.displays;
    }

    std::vector<sys::Screen> out;
    std::map<uint32_t, sys::ScreenInsets> insets;
    bool insets_read = false;
    for (const auto& d : displays) {
        // A sleeping display shows nothing and a mirror shows another
        // display's space: neither is a monitor of its own.
        if (!d.is_active || !d.mirror_of.empty()) continue;
        char* end = nullptr;
        unsigned long id = std::strtoul(d.device_name.c_str(), &end, 10);  // the CGDirectDisplayID
        if (d.device_name.empty() || *end != '\0') continue;
        if (!insets_read) {
            insets = sys::screen_insets();
            insets_read = true;
        }
        sys::Screen sc;
        sc.display_id = uint32_t(id);
        sc.frame = Rect{d.geometry.x, d.geometry.y, int32_t(d.geometry.width), int32_t(d.geometry.height)};
        sc.visible = sc.frame;
        sc.scale = d.scale.factor > 0 ? d.scale.factor : 1.0;
        sc.primary = d.is_primary;
        auto in = insets.find(sc.display_id);
        if (in != insets.end()) {
            sc.name = in->second.name;
            const auto& i = in->second;
            // Insets larger than half the display are from a stale, larger
            // configuration: ignore them.
            if (i.left + i.right < sc.frame.width / 2.0 && i.top + i.bottom < sc.frame.height / 2.0) {
                int32_t l = int32_t(std::lround(i.left)), t = int32_t(std::lround(i.top));
                int32_t r = int32_t(std::lround(i.right)), b = int32_t(std::lround(i.bottom));
                sc.visible = Rect{sc.frame.x + l, sc.frame.y + t, sc.frame.width - l - r, sc.frame.height - t - b};
            }
        }
        if (sc.name.empty()) sc.name = d.name.empty() ? "display-" + std::to_string(id) : d.name;
        out.push_back(std::move(sc));
    }
    return out;
}

}  // namespace brocompositor::mac
