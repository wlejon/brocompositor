#pragma once

#include "wl_types.h"
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace brocompositor {

struct WlOutputInfo {
    MonitorId monitor_id = PrimaryMonitorId;
    std::string name = "WL-1";
    Rect bounds{0, 0, 1920, 1080};
    double scale = 1.0;
    int refresh_rate_hz = 60;
    bool enabled = true;
};

class WlBackend {
public:
    enum class BackendMode {
        Auto,
        DrmKms,
        Nested,
        Headless
    };

    struct Config {
        BackendMode mode = BackendMode::Auto;
        std::string socket_name = "wayland-bro-0";
        bool start_thread = true;
        std::vector<WlOutputInfo> initial_outputs;
    };

    using OutputCallback = std::function<void(MonitorId, bool added, const WlOutputInfo*)>;
    using Task = std::function<void()>;

    WlBackend();
    ~WlBackend();

    WlBackend(const WlBackend&) = delete;
    WlBackend& operator=(const WlBackend&) = delete;

    bool initialize(const Config& config = {});
    void shutdown();
    bool is_running() const { return is_running_.load(); }

    // Underlying Wayland pointers (valid on Linux wlroots runtime)
    wl_display* get_display() const { return display_; }
    wlr_backend* get_backend() const { return backend_; }
    const std::string& get_socket_name() const { return socket_name_; }
    BackendMode get_mode() const { return mode_; }

    // Output management
    void add_output(const WlOutputInfo& info);
    void remove_output(MonitorId monitor_id);
    bool set_output_bounds(MonitorId monitor_id, const Rect& bounds);
    bool set_output_scale(MonitorId monitor_id, double scale);
    const WlOutputInfo* get_output(MonitorId monitor_id) const;
    std::vector<MonitorId> get_outputs() const;

    // Output listeners
    void set_output_callback(OutputCallback cb);

    // Thread dispatch
    void dispatch_on_loop(Task task);
    void process_pending();

private:
    void loop_thread_func();

    std::atomic<bool> is_running_{false};
    std::atomic<bool> stop_requested_{false};
    BackendMode mode_ = BackendMode::Auto;
    std::string socket_name_;

    wl_display* display_ = nullptr;
    wlr_backend* backend_ = nullptr;
    wlr_renderer* renderer_ = nullptr;
    wlr_allocator* allocator_ = nullptr;

    std::thread loop_thread_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<Task> task_queue_;

    mutable std::mutex output_mutex_;
    std::unordered_map<MonitorId, WlOutputInfo> outputs_;
    OutputCallback output_cb_;
};

} // namespace brocompositor
