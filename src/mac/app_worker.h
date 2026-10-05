// One thread per managed application, owning everything that talks to it
// through Accessibility: the AX application element (with a messaging
// timeout), an AXObserver on the thread's own run loop, and a serial queue of
// window operations. A hung application blocks its own worker for at most a
// messaging timeout per call and never anything else.
//
// The worker publishes what AX knows about the application's windows
// (minimized, fullscreen, title, subrole, on another Space) for the
// backend's tracking thread, rescanning when the application reports a
// window created, destroyed, resized, retitled or (de)minimized or when the
// tracking thread asks, and calls `changed` whenever anything about its
// windows may have changed (moves included).
//
// AX hands out a new element for a window after some transitions (a
// deminiaturized window is destroyed and re-created as far as AX is
// concerned, with the same CGWindowID), so notifications are subscribed per
// element, not per window id: a scan that finds a different element for a
// known window subscribes the new one.
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
        std::chrono::milliseconds timeout{1000};  // set on every element
        ax::Element app;
        // CGWindowID -> AX window (also those on another Space, whose
        // elements still work although AXWindows no longer lists them).
        std::map<uint32_t, ax::Element> windows;
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
    // The application's focused (key) window as of the last scan, 0 for none.
    uint32_t focused() const { return focused_.load(); }
    // At least one scan completed (windows() is meaningful).
    bool scanned() const { return scans_.load() > 0; }
    // Completed scans so far: a snapshot read after the count moved past a
    // remembered value reflects the application's state after that moment.
    uint64_t scans() const { return scans_.load(); }
    std::chrono::steady_clock::time_point started() const { return started_; }
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
    void subscribe(const ax::Element& window, uint32_t cgid);
    void unsubscribe(uint32_t cgid);
    static void on_source(void* info);
    static void on_notification(AXObserverRef, AXUIElementRef element, CFStringRef notification, void* refcon);

    const uint32_t pid_;
    const std::chrono::milliseconds timeout_;
    const std::function<void(uint32_t)> changed_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    mutable std::mutex mutex_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    bool rescan_requested_ = false;
    CFRunLoopRef run_loop_ = nullptr;  // retained while the thread runs
    CFRunLoopSourceRef source_ = nullptr;
    std::map<uint32_t, ax::WindowInfo> published_;

    std::atomic<uint64_t> scans_{0};
    std::atomic<uint32_t> focused_{0};
    std::atomic<bool> unresponsive_{false};

    // Worker thread only.
    Context ctx_;
    CFRef<AXObserverRef> observer_;
    std::map<uint32_t, ax::Element> subscribed_;  // the element each window's notifications come from
    std::map<uint32_t, ax::WindowInfo> known_;    // the last scan's facts (other-Space windows keep theirs)
};

}  // namespace brocompositor::mac
