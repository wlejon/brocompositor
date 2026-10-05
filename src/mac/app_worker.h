// One thread per managed application, owning everything that talks to it
// through Accessibility: the AX application element (with a messaging
// timeout), an AXObserver on the thread's own run loop, and a serial queue of
// window operations. A hung application blocks its own worker for at most a
// messaging timeout per call and never anything else.
//
// The worker publishes what AX knows about the application's windows
// (minimized, fullscreen, title, subrole) for the backend's tracking thread,
// rescanning when the application reports a window created, destroyed,
// retitled or (de)minimized, and calls `changed` whenever anything about its
// windows may have changed (moves and resizes included).
#pragma once

#include "mac/ax.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace brocompositor::mac {

class AppWorker : public std::enable_shared_from_this<AppWorker> {
public:
    // Valid only on the worker thread, inside a job.
    struct Context {
        uint32_t pid = 0;
        ax::Element app;
        std::map<uint32_t, ax::Element> windows;  // CGWindowID -> AX window
        // The AX window for a window-server id, rescanning once if unknown.
        AXUIElementRef find(uint32_t cgid);
        AppWorker* worker = nullptr;
    };
    // A job runs with the context, or with nullptr when the worker stopped
    // before it could run (complete it with a failure then).
    using Job = std::function<void(Context*)>;

    static std::shared_ptr<AppWorker> start(uint32_t pid, std::chrono::milliseconds timeout,
                                            std::function<void(uint32_t pid)> changed);
    ~AppWorker();

    uint32_t pid() const { return pid_; }
    // False after stop() (the job is not run at all then).
    bool post(Job job);
    void request_rescan();
    std::map<uint32_t, ax::WindowInfo> windows() const;
    // At least one scan completed (windows() is meaningful).
    bool scanned() const { return scanned_.load(); }
    // The last AX call into the application timed out.
    bool unresponsive() const { return unresponsive_.load(); }
    void note(AXError e);  // records responsiveness from a job's AX result

    // Stops after the running job (if any) returns; queued jobs run with
    // nullptr. Never blocks.
    void stop();

private:
    AppWorker(uint32_t pid, std::chrono::milliseconds timeout, std::function<void(uint32_t)> changed)
        : pid_(pid), timeout_(timeout), changed_(std::move(changed)) {}
    void run(std::shared_ptr<AppWorker> self);
    void drain();
    void rescan();
    void subscribe(AXUIElementRef window, uint32_t cgid);
    static void on_source(void* info);
    static void on_notification(AXObserverRef, AXUIElementRef element, CFStringRef notification, void* refcon);

    const uint32_t pid_;
    const std::chrono::milliseconds timeout_;
    const std::function<void(uint32_t)> changed_;

    mutable std::mutex mutex_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    bool rescan_requested_ = false;
    CFRunLoopRef run_loop_ = nullptr;  // retained while the thread runs
    CFRunLoopSourceRef source_ = nullptr;
    std::map<uint32_t, ax::WindowInfo> published_;

    std::atomic<bool> scanned_{false};
    std::atomic<bool> unresponsive_{false};

    // Worker thread only.
    Context ctx_;
    CFRef<AXObserverRef> observer_;
    std::set<uint32_t> subscribed_;
};

}  // namespace brocompositor::mac
