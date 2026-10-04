#include "wl_backend.h"
#include <iostream>

namespace brocompositor {

WlBackend::WlBackend() = default;

WlBackend::~WlBackend() {
    shutdown();
}

bool WlBackend::initialize(const Config& config) {
    if (is_running_.load()) return true;

    mode_ = config.mode;
    socket_name_ = config.socket_name;
    stop_requested_.store(false);

#if defined(BRO_HAS_WAYLAND)
    display_ = wl_display_create();
    if (!display_) {
        std::cerr << "[WlBackend] Failed to create wl_display\n";
        return false;
    }

    backend_ = wlr_backend_autocreate(display_);
    if (!backend_) {
        std::cerr << "[WlBackend] Failed to autocreate wlr_backend\n";
        wl_display_destroy(display_);
        display_ = nullptr;
        return false;
    }

    renderer_ = wlr_renderer_autocreate(backend_);
    if (renderer_) {
        wlr_renderer_init_wl_display(renderer_, display_);
        allocator_ = wlr_allocator_autocreate(backend_, renderer_);
    }

    const char* socket = wl_display_add_socket_auto(display_);
    if (socket) {
        socket_name_ = socket;
    } else if (!socket_name_.empty()) {
        wl_display_add_socket(display_, socket_name_.c_str());
    }
#else
    // Simulated mock backend for Windows / cross-platform builds
    display_ = reinterpret_cast<wl_display*>(static_cast<uintptr_t>(0x1000));
    backend_ = reinterpret_cast<wlr_backend*>(static_cast<uintptr_t>(0x2000));
#endif

    // Setup initial outputs
    if (!config.initial_outputs.empty()) {
        for (const auto& out : config.initial_outputs) {
            add_output(out);
        }
    } else {
        // Default output if none provided
        WlOutputInfo primary;
        primary.monitor_id = PrimaryMonitorId;
        primary.name = "WL-1";
        primary.bounds = {0, 0, 1920, 1080};
        primary.scale = 1.0;
        primary.refresh_rate_hz = 60;
        primary.enabled = true;
        add_output(primary);
    }

    is_running_.store(true);

    if (config.start_thread) {
        loop_thread_ = std::thread(&WlBackend::loop_thread_func, this);
    }

    return true;
}

void WlBackend::shutdown() {
    if (!is_running_.load()) return;

    stop_requested_.store(true);
    queue_cv_.notify_all();

#if defined(BRO_HAS_WAYLAND)
    if (display_) {
        wl_display_terminate(display_);
    }
#endif

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }

#if defined(BRO_HAS_WAYLAND)
    if (allocator_) {
        wlr_allocator_destroy(allocator_);
        allocator_ = nullptr;
    }
    if (renderer_) {
        wlr_renderer_destroy(renderer_);
        renderer_ = nullptr;
    }
    if (backend_) {
        wlr_backend_destroy(backend_);
        backend_ = nullptr;
    }
    if (display_) {
        wl_display_destroy(display_);
        display_ = nullptr;
    }
#else
    display_ = nullptr;
    backend_ = nullptr;
#endif

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        outputs_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!task_queue_.empty()) {
            task_queue_.pop();
        }
    }

    is_running_.store(false);
}

void WlBackend::add_output(const WlOutputInfo& info) {
    OutputCallback cb_to_invoke = nullptr;
    WlOutputInfo copied_info;
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        outputs_[info.monitor_id] = info;
        copied_info = info;
        cb_to_invoke = output_cb_;
    }

    if (cb_to_invoke) {
        cb_to_invoke(info.monitor_id, true, &copied_info);
    }
}

void WlBackend::remove_output(MonitorId monitor_id) {
    OutputCallback cb_to_invoke = nullptr;
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        auto it = outputs_.find(monitor_id);
        if (it != outputs_.end()) {
            outputs_.erase(it);
            cb_to_invoke = output_cb_;
        }
    }

    if (cb_to_invoke) {
        cb_to_invoke(monitor_id, false, nullptr);
    }
}

bool WlBackend::set_output_bounds(MonitorId monitor_id, const Rect& bounds) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    auto it = outputs_.find(monitor_id);
    if (it != outputs_.end()) {
        it->second.bounds = bounds;
        return true;
    }
    return false;
}

bool WlBackend::set_output_scale(MonitorId monitor_id, double scale) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    auto it = outputs_.find(monitor_id);
    if (it != outputs_.end()) {
        it->second.scale = scale;
        return true;
    }
    return false;
}

const WlOutputInfo* WlBackend::get_output(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(output_mutex_);
    auto it = outputs_.find(monitor_id);
    return it != outputs_.end() ? &it->second : nullptr;
}

std::vector<MonitorId> WlBackend::get_outputs() const {
    std::lock_guard<std::mutex> lock(output_mutex_);
    std::vector<MonitorId> ids;
    ids.reserve(outputs_.size());
    for (const auto& pair : outputs_) {
        ids.push_back(pair.first);
    }
    return ids;
}

void WlBackend::set_output_callback(OutputCallback cb) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_cb_ = std::move(cb);
}

void WlBackend::dispatch_on_loop(Task task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

void WlBackend::process_pending() {
    std::queue<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks.swap(task_queue_);
    }

    while (!tasks.empty()) {
        auto& t = tasks.front();
        if (t) {
            t();
        }
        tasks.pop();
    }
}

void WlBackend::loop_thread_func() {
    while (!stop_requested_.load()) {
#if defined(BRO_HAS_WAYLAND)
        if (display_) {
            wl_event_loop* loop = wl_display_get_event_loop(display_);
            if (loop) {
                wl_event_loop_dispatch(loop, 10);
            }
        }
#endif
        process_pending();

        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
            return stop_requested_.load() || !task_queue_.empty();
        });
    }
}

} // namespace brocompositor
