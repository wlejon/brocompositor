#include "mac/app_worker.h"

#include "mac/system.h"

#include <thread>

namespace brocompositor::mac {

namespace {

const CFStringRef kAppNotifications[] = {kAXWindowCreatedNotification, kAXFocusedWindowChangedNotification,
                                         kAXApplicationHiddenNotification, kAXApplicationShownNotification};
const CFStringRef kWindowNotifications[] = {kAXUIElementDestroyedNotification, kAXMovedNotification,
                                            kAXResizedNotification, kAXWindowMiniaturizedNotification,
                                            kAXWindowDeminiaturizedNotification, kAXTitleChangedNotification};

}  // namespace

AXUIElementRef AppWorker::Context::find(uint32_t cgid) {
    auto it = windows.find(cgid);
    if (it != windows.end()) return it->second.get();
    worker->rescan();
    it = windows.find(cgid);
    return it == windows.end() ? nullptr : it->second.get();
}

std::shared_ptr<AppWorker> AppWorker::start(uint32_t pid, std::chrono::milliseconds timeout,
                                            std::function<void(uint32_t)> changed) {
    std::shared_ptr<AppWorker> w(new AppWorker(pid, timeout, std::move(changed)));
    // Detached: a worker blocked in a call into a hung application outlives
    // its backend safely (it holds itself), and finishes once the call
    // times out.
    std::thread([w] { w->run(w); }).detach();
    return w;
}

AppWorker::~AppWorker() = default;

bool AppWorker::post(Job job) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    jobs_.push_back(std::move(job));
    if (source_) {
        CFRunLoopSourceSignal(source_);
        CFRunLoopWakeUp(run_loop_);
    }
    return true;
}

void AppWorker::request_rescan() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    rescan_requested_ = true;
    if (source_) {
        CFRunLoopSourceSignal(source_);
        CFRunLoopWakeUp(run_loop_);
    }
}

std::map<uint32_t, ax::WindowInfo> AppWorker::windows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

void AppWorker::note(AXError e) {
    if (ax::unresponsive(e)) unresponsive_ = true;
    else if (e == kAXErrorSuccess) unresponsive_ = false;
}

void AppWorker::stop() {
    std::deque<Job> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        dropped.swap(jobs_);
        if (run_loop_) CFRunLoopStop(run_loop_);
    }
    for (auto& j : dropped) j(nullptr);
}

void AppWorker::on_source(void* info) { static_cast<AppWorker*>(info)->drain(); }

void AppWorker::drain() {
    for (;;) {
        Job job;
        bool rescan_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            } else if (rescan_requested_) {
                rescan_requested_ = false;
                rescan_now = true;
            } else {
                return;
            }
        }
        if (rescan_now) rescan();
        else job(&ctx_);
    }
}

void AppWorker::rescan() {
    AXError err = kAXErrorSuccess;
    std::vector<ax::Element> list = ax::windows(ctx_.app.get(), timeout_, &err);
    note(err);
    sys::trace("pid %u rescan: AXWindows err %d, %zu windows", pid_, int(err), list.size());
    if (err != kAXErrorSuccess && err != kAXErrorNoValue) {
        // Unresponsive or refused: keep what was known.
        if (!scanned()) changed_(pid_);
        return;
    }
    std::map<uint32_t, ax::Element> elements;
    std::map<uint32_t, ax::WindowInfo> infos;
    for (ax::Element& w : list) {
        AXError e = kAXErrorSuccess;
        auto i = ax::info(w.get(), &e);
        if (ax::unresponsive(e)) {
            note(e);
            return;
        }
        if (!i || i->cgid == 0) continue;
        sys::trace("  cgid %u '%s' %s min %d fs %d %d,%d %dx%d", i->cgid, i->title.c_str(), i->subrole.c_str(),
                   i->minimized, i->fullscreen, i->frame.x, i->frame.y, i->frame.width, i->frame.height);
        auto sub = subscribed_.find(i->cgid);
        if (sub == subscribed_.end() || !CFEqual(sub->second.get(), w.get())) subscribe(w, i->cgid);
        elements.emplace(i->cgid, w);
        infos.emplace(i->cgid, *i);
    }
    // Windows AXWindows stopped listing: closed / ordered out (the element
    // is invalid), or on a Space that is not active (it still answers).
    for (auto& [cgid, element] : ctx_.windows) {
        if (elements.count(cgid)) continue;
        AXError e = ax::probe(element.get());
        if (ax::unresponsive(e)) {
            note(e);
            return;
        }
        auto known = known_.find(cgid);
        if (e != kAXErrorSuccess || known == known_.end()) continue;
        ax::WindowInfo i = known->second;
        if (auto fresh = ax::info(element.get(), nullptr)) i = *fresh;
        i.cgid = cgid;
        i.other_space = true;
        sys::trace("  cgid %u on another Space", cgid);
        elements.emplace(cgid, element);
        infos.emplace(cgid, i);
    }
    for (auto it = subscribed_.begin(); it != subscribed_.end();) {
        if (elements.count(it->first)) {
            ++it;
            continue;
        }
        unsubscribe(it->first);  // erases
        it = subscribed_.lower_bound(it->first);
    }
    AXError fe = kAXErrorSuccess;
    uint32_t focused = ax::focused_window(ctx_.app.get(), &fe);
    if (ax::unresponsive(fe)) {
        note(fe);
        return;
    }
    focused_ = focused;
    ctx_.windows = std::move(elements);
    known_ = infos;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        published_ = std::move(infos);
    }
    ++scans_;
    changed_(pid_);
}

void AppWorker::subscribe(const ax::Element& window, uint32_t cgid) {
    if (!observer_) return;
    unsubscribe(cgid);
    for (CFStringRef n : kWindowNotifications) {
        AXError e = AXObserverAddNotification(observer_.get(), window.get(), n, this);
        if (e != kAXErrorSuccess) sys::trace("pid %u cgid %u observe window notification: err %d", pid_, cgid, int(e));
    }
    subscribed_[cgid] = window;
}

void AppWorker::unsubscribe(uint32_t cgid) {
    auto it = subscribed_.find(cgid);
    if (it == subscribed_.end()) return;
    // Fails harmlessly for an element that is already gone.
    if (observer_)
        for (CFStringRef n : kWindowNotifications) AXObserverRemoveNotification(observer_.get(), it->second.get(), n);
    subscribed_.erase(it);
}

void AppWorker::on_notification(AXObserverRef, AXUIElementRef, CFStringRef notification, void* refcon) {
    auto* self = static_cast<AppWorker*>(refcon);
    if (sys::tracing()) {
        char name[128] = {};
        CFStringGetCString(notification, name, sizeof(name), kCFStringEncodingUTF8);
        sys::trace("pid %u notification %s", self->pid_, name);
    }
    // Geometry comes from the window server's list, so a move needs no AX
    // reads (and drags send many). A resize may be a fullscreen transition
    // (AXFullScreen), so it rescans, as everything else does.
    if (CFEqual(notification, kAXMovedNotification))
        self->changed_(self->pid_);
    else
        self->request_rescan();
}

void AppWorker::run(std::shared_ptr<AppWorker> self) {
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    CFRunLoopSourceContext sc{};
    sc.info = this;
    sc.perform = &AppWorker::on_source;
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(nullptr, 0, &sc);
    CFRunLoopAddSource(rl, source, kCFRunLoopDefaultMode);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CFRetain(rl);
        run_loop_ = rl;
        source_ = source;
        rescan_requested_ = true;
        CFRunLoopSourceSignal(source_);
    }

    ctx_.pid = pid_;
    ctx_.timeout = timeout_;
    ctx_.worker = this;
    ctx_.app = ax::application(pid_, timeout_);
    AXObserverRef obs = nullptr;
    if (ctx_.app && AXObserverCreate(pid_t(pid_), &AppWorker::on_notification, &obs) == kAXErrorSuccess) {
        observer_ = CFRef<AXObserverRef>(obs);
        CFRunLoopAddSource(rl, AXObserverGetRunLoopSource(obs), kCFRunLoopDefaultMode);
        for (CFStringRef n : kAppNotifications) {
            AXError e = AXObserverAddNotification(obs, ctx_.app.get(), n, this);
            sys::trace("pid %u observe app notification: err %d", pid_, int(e));
            note(e);
            if (ax::unresponsive(e)) break;
        }
    }

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
    }

    if (observer_) CFRunLoopRemoveSource(rl, AXObserverGetRunLoopSource(observer_.get()), kCFRunLoopDefaultMode);
    observer_ = CFRef<AXObserverRef>();
    subscribed_.clear();
    known_.clear();
    ctx_ = Context{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run_loop_ = nullptr;
        source_ = nullptr;
    }
    CFRunLoopSourceInvalidate(source);
    CFRelease(source);
    CFRelease(rl);
    self.reset();
}

}  // namespace brocompositor::mac
