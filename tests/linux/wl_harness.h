// A test host for the Wayland server role: owns a ServerBackend and a
// WindowManager, drains both queues on its own thread, routes input with
// simple hit testing, and composites every output on the CPU (shm / linear
// images) from the client surfaces it leases, presenting at the output's
// frame pace. Everything it observed is logged for the tests to wait on.
#pragma once

#include "brocompositor/linux/cpu_mapping.h"
#include "brocompositor/linux/server.h"
#include "brocompositor/window_manager.h"

#include "check.h"
#include "linux/child.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bctest {

// Gives this test process a private XDG_RUNTIME_DIR and clears any parent
// display variables. Returns the directory.
std::string private_runtime_dir();

// Types text through the server's virtual keyboard (US layout: letters,
// digits, space, '/', '-', '.', '_', '>' and '\n').
void type_text(brocompositor::wl::ServerBackend& server, const std::string& text);
void tap_key(brocompositor::wl::ServerBackend& server, uint32_t evdev_key);
constexpr uint32_t kKeyA = 30, kKeyB = 48, kKeyC = 46, kKeyEnter = 28;

struct HostOptions {
    brocompositor::wl::ServerConfig server;
    bool composite = true;          // draw windows/layers into the outputs (CPU)
    bool route_input = true;        // hit-test pointer input, forward keys to the focused window
    bool focus_new_windows = true;  // FocusWindow on WindowAdded
    bool present = true;            // render + present on OutputFrame
    uint32_t background = 0xFF203040;
    uint32_t lock_background = 0xFF000000;
    // Draws only lock surfaces while the session is locked. false plays a
    // naive host that keeps drawing windows (the server must withhold them).
    bool lock_aware = true;
    bool honor_requests = true;   // WindowRequest Activate / Close / (Un)Minimize / (Un)Maximize / (Un)Fullscreen
    bool answer_captures = true;  // CaptureRequest: CPU copy, then capture_done()
    // Presents the bounding box of what changed since the last composite
    // ({Rect{}} when nothing did) instead of full damage.
    bool track_damage = false;
    // Called on the host thread after each composite, before present (for
    // GPU hosts that want to render the image themselves).
    std::function<void(brocompositor::MonitorId, const brocompositor::SharedImage&,
                       brocompositor::wl::PresentRequest&)>
        render_hook;
};

class Host {
public:
    bool start(HostOptions options, std::string* error);
    ~Host();
    void stop();

    brocompositor::wl::ServerBackend& server() { return *server_; }
    // WAYLAND_DISPLAY + XDG_RUNTIME_DIR, DISPLAY = XWayland's (unset without it).
    std::vector<std::string> client_env() const;

    // Waits (on the log) until `pred` holds; true when it did in time.
    bool wait(const std::function<bool()>& pred, int timeout_ms = 5000);
    // Copies of everything observed so far.
    std::vector<brocompositor::Event> events() const;
    std::vector<brocompositor::wl::ServerEvent> server_events() const;

    template <class T>
    std::vector<T> events_of() const {
        std::vector<T> out;
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& e : events_)
            if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
        return out;
    }
    template <class T>
    std::vector<T> server_events_of() const {
        std::vector<T> out;
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& e : sevents_)
            if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
        return out;
    }

    // The window with this app_id (most recent), kNoWindow when none.
    brocompositor::WindowId window_by_app_id(const std::string& app_id) const;
    std::optional<brocompositor::WindowSnapshot> wm_window(brocompositor::WindowId id) const;
    brocompositor::WindowId wm_focused() const;
    std::vector<brocompositor::MonitorSnapshot> wm_monitors() const;
    // Runs a WM action on the host thread and executes its commands.
    void wm_do(const std::function<std::vector<brocompositor::Command>(brocompositor::WindowManager&)>& action);

    // Pixel (0xAARRGGBB) of a surface's newest frame, via a CPU mapping.
    std::optional<uint32_t> surface_pixel(brocompositor::wl::SurfaceId surface, uint32_t x, uint32_t y);
    // Pixel of the image last presented on an output (shm / linear only).
    std::optional<uint32_t> output_pixel(brocompositor::MonitorId output, uint32_t x, uint32_t y);
    uint64_t presents(brocompositor::MonitorId output) const;
    // Surfaces drawn (and so sent frame callbacks) on the last present.
    std::set<brocompositor::wl::SurfaceId> last_drawn(brocompositor::MonitorId output) const;
    // Stops / resumes rendering on OutputFrame; resuming schedules a frame on
    // every output (nothing else would wake an idle output).
    void set_presenting(bool on);
    uint64_t captures_answered() const {
        std::lock_guard<std::mutex> lock(m_);
        return captures_answered_;
    }

private:
    void run();
    void handle(const brocompositor::Event& e);
    void handle(const brocompositor::wl::ServerEvent& e);
    void render(brocompositor::MonitorId output);
    std::vector<brocompositor::Rect> diff_damage(brocompositor::MonitorId output,
                                                 const brocompositor::wl::CpuMapping& map);
    void route_pointer(double x, double y, uint32_t time);
    // Topmost input surface at a layout point and its layout origin.
    struct Pick {
        brocompositor::wl::SurfaceId surface = 0;
        double ox = 0, oy = 0;
    };
    Pick pick(double x, double y);
    void handle_input(const brocompositor::wl::ServerEvent& e);
    void handle_request(const brocompositor::wl::WindowRequest& r);
    void answer_capture(const brocompositor::wl::CaptureRequest& r);
    void blit_tree(brocompositor::wl::CpuMapping& dst, const brocompositor::Rect& out_layout, float scale,
                   brocompositor::Point origin, const std::vector<brocompositor::wl::SurfaceNode>& tree,
                   std::vector<brocompositor::wl::SurfaceId>& drawn);

    HostOptions options_;
    std::unique_ptr<brocompositor::wl::ServerBackend> server_;
    std::unique_ptr<brocompositor::WindowManager> wm_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> presenting_{true};
    std::mutex wake_m_;
    std::condition_variable wake_cv_;
    bool woken_ = false;

    mutable std::mutex m_;
    std::condition_variable log_cv_;
    std::vector<brocompositor::Event> events_;
    std::vector<brocompositor::wl::ServerEvent> sevents_;
    std::map<brocompositor::MonitorId, uint64_t> presents_;
    std::map<brocompositor::MonitorId, brocompositor::SharedImage> last_image_;
    std::map<brocompositor::MonitorId, std::vector<uint32_t>> last_pixels_;  // track_damage, host thread
    std::map<brocompositor::MonitorId, std::set<brocompositor::wl::SurfaceId>> last_drawn_;
    std::vector<std::function<void()>> host_jobs_;
    std::vector<brocompositor::wl::OutputInfo> outputs_;
    brocompositor::wl::SurfaceId pointer_surface_ = 0;
    uint64_t captures_answered_ = 0;
    std::map<int32_t, Pick> touch_points_;  // host thread
    std::map<brocompositor::wl::TabletToolId, Pick> tool_focus_;
};

}  // namespace bctest
