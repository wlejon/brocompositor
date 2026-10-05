// Cross-thread job queue into the server thread's wl_event_loop, signalled
// through an eventfd. Hosts call into the server from any thread; every
// wlroots call happens on the server thread.
#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace brocompositor::wl {

class Dispatcher {
public:
    Dispatcher();
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    // Queues a job; false once closed (the job is dropped).
    bool post(std::function<void()> job);
    // Server thread: drains the eventfd and runs every queued job.
    void run_pending();
    // Stops accepting jobs and drops the queued ones (their callers' futures
    // break, so call() returns a default value).
    void close();

    void bind_thread() { thread_ = std::this_thread::get_id(); }
    bool on_thread() const { return std::this_thread::get_id() == thread_; }

    // Runs `fn` on the server thread and waits for its result. Returns a
    // default-constructed value if the server is gone.
    template <class F>
    auto call(F&& fn) -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        if (on_thread()) return fn();
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        bool queued = post([promise, f = std::forward<F>(fn)]() mutable {
            if constexpr (std::is_void_v<R>) {
                f();
                promise->set_value();
            } else {
                promise->set_value(f());
            }
        });
        if (!queued) {
            if constexpr (std::is_void_v<R>) return;
            else return R{};
        }
        try {
            return future.get();
        } catch (const std::future_error&) {
            if constexpr (std::is_void_v<R>) return;
            else return R{};
        }
    }

private:
    int fd_ = -1;
    std::mutex m_;
    bool closed_ = false;
    std::vector<std::function<void()>> jobs_;
    std::thread::id thread_;
};

}  // namespace brocompositor::wl
