#include "win/shell_impl.h"
#include "win/util.h"

#include <shlobj.h>

#include <filesystem>
#include <future>

namespace brocompositor::win {

namespace {

void register_classes() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = module_instance();
        wc.lpfnWndProc = listener_proc;
        wc.lpszClassName = L"brocompositor.listener";
        RegisterClassExW(&wc);
        wc.lpfnWndProc = appbar_proc;
        wc.lpszClassName = L"brocompositor.appbar";
        RegisterClassExW(&wc);
    });
}

void shell_thread(ShellBackend::Impl* d, std::promise<std::string>* ready) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    register_classes();
    // A hidden top-level (not message-only) window: only top-level windows
    // receive WM_SETTINGCHANGE broadcasts (SPI_SETWORKAREA). Display
    // topology changes come from brodisplays, posted here as kMsgDisplays.
    HWND listener = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"brocompositor.listener", L"",
                                    WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module_instance(), nullptr);
    if (!listener) {
        ready->set_value("CreateWindowEx(listener) failed: " + std::to_string(GetLastError()));
        return;
    }
    SetWindowLongPtrW(listener, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->listener = listener;
    }
    auto fail = [&](std::string why) {
        d->monitors.stop();
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            d->listener = nullptr;
        }
        DestroyWindow(listener);
        ready->set_value(std::move(why));
    };
    std::string err;
    if (!d->monitors.start([d] { d->request_displays(); }, &err)) {
        fail(err);
        return;
    }
    if (!d->install_hooks()) {
        fail("SetWinEventHook failed");
        return;
    }
    ready->set_value(std::string());
    ready = nullptr;

    d->report_initial_state();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    d->remove_hooks();
    HWND l = d->listener;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->listener = nullptr;
    }
    d->monitors.stop();
    // Jobs posted after the loop ended are dropped (their owners hold
    // promises that then report failure through their own fallbacks).
    while (PeekMessageW(&msg, l, kMsgCall, kMsgCall, PM_REMOVE))
        delete reinterpret_cast<std::function<void()>*>(msg.lParam);
    DestroyWindow(l);
}

std::string default_journal_dir() {
    PWSTR base = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
        out = (std::filesystem::path(base) / "brocompositor" / "journal").string();
    CoTaskMemFree(base);
    return out;
}

}  // namespace

LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<ShellBackend::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (d) {
        switch (msg) {
            case kMsgCall: {
                std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
                (*fn)();
                return 0;
            }
            case kMsgFlush:
                d->flush();
                return 0;
            case kMsgDirty:
                d->mark_dirty(reinterpret_cast<HWND>(lp));
                return 0;
            case kMsgDisplays:
                d->monitors.take_changes();
                d->report_monitors_if_changed();
                return 0;
            case WM_SETTINGCHANGE:
                if (wp == SPI_SETWORKAREA) d->report_monitors_if_changed();
                break;
            default: break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShellBackend::Impl::post(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    std::lock_guard<std::mutex> lock(mutex);
    if (!listener || !PostMessageW(listener, kMsgCall, 0, reinterpret_cast<LPARAM>(p))) {
        delete p;
        return false;
    }
    return true;
}

void ShellBackend::Impl::call(const std::function<void()>& fn) {
    if (on_shell_thread()) {
        fn();
        return;
    }
    std::promise<void> done;
    auto f = done.get_future();
    if (!post([&] {
            fn();
            done.set_value();
        }))
        return;
    f.wait();
}

bool ShellBackend::Impl::in_scope(HWND hwnd) const {
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (config.process_filter.empty()) return true;
    for (uint32_t p : config.process_filter)
        if (p == pid) return true;
    return false;
}

uint64_t process_start_time(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    uint64_t t = 0;
    if (GetProcessTimes(p, &created, &exited, &kernel, &user))
        t = uint64_t(created.dwHighDateTime) << 32 | created.dwLowDateTime;
    CloseHandle(p);
    return t;
}

// ---------------------------------------------------------------- public

ShellBackend::ShellBackend(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<ShellBackend> ShellBackend::create(const ShellConfig& config, std::string* error) {
    auto impl = std::make_shared<Impl>();
    impl->config = config;
    shell::SerialWorkers::Options wo;
    wo.thread_init = [] { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); };
    impl->workers = shell::SerialWorkers::create(std::move(wo));
    std::string dir = config.journal_dir.empty() ? default_journal_dir() : config.journal_dir;
    if (dir == "-") dir.clear();
    DWORD self = GetCurrentProcessId();
    impl->journal = std::make_unique<shell::Journal>(dir, self, process_start_time(self));

    std::promise<std::string> ready;
    auto result = ready.get_future();
    Impl* d = impl.get();
    impl->thread = std::thread([d, &ready] { shell_thread(d, &ready); });
    impl->thread_id = GetThreadId(impl->thread.native_handle());
    std::string err = result.get();
    if (!err.empty()) {
        impl->thread.join();
        impl->workers->shutdown(std::chrono::milliseconds(0));
        if (error) *error = err;
        return nullptr;
    }

    // Recovery of dead instances' journals runs on an op worker of its own:
    // restoring a window may block on its (possibly hung) application.
    auto promise = std::make_shared<std::promise<RecoveryReport>>();
    impl->recovery = promise->get_future().share();
    if (!config.recover || !impl->journal->enabled() ||
        !impl->workers->post(0, [self = impl, promise] { promise->set_value(self->recover_stale()); }))
        promise->set_value(RecoveryReport{});
    return std::unique_ptr<ShellBackend>(new ShellBackend(std::move(impl)));
}

ShellBackend::~ShellBackend() {
    // Put hidden windows back (bounded: a hung application keeps its window
    // in the journal for the next instance), give the edges back, stop.
    auto restored = restore_all();
    auto deadline = std::chrono::steady_clock::now() + impl_->config.shutdown_timeout;
    restored.wait_until(deadline);
    impl_->call([d = impl_.get()] { d->release_all(); });
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    impl_->workers->shutdown(std::max(left, std::chrono::milliseconds(100)));
    // Whatever is still hidden (a stuck worker) stays journaled; everything
    // else was undone, so a clean shutdown leaves no journal behind.
    impl_->journal_sync();
}

EventQueue& ShellBackend::events() { return impl_->queue; }

bool ShellBackend::execute(const Command& command) {
    WindowId id = std::visit([](const auto& c) { return c.id; }, command);
    bool focus_none = std::holds_alternative<FocusWindow>(command) && id == kNoWindow;
    if (!focus_none && !impl_->hwnd_of(id)) return false;
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, PlaceWindow>) place(c.id, c.frame);
            else if constexpr (std::is_same_v<T, SetWindowVisible>) set_visible(c.id, c.visible);
            else if constexpr (std::is_same_v<T, FocusWindow>) focus(c.id);
            else if constexpr (std::is_same_v<T, CloseWindow>) close(c.id);
        },
        command);
    return true;
}

size_t ShellBackend::execute(const std::vector<Command>& commands) {
    size_t refused = 0;
    for (const auto& c : commands) refused += execute(c) ? 0 : 1;
    return refused;
}

Completion<bool> ShellBackend::place(WindowId id, const Rect& frame) {
    HWND h = impl_->hwnd_of(id);
    if (!h || frame.empty()) return completed(false);
    return impl_->run_for<bool>(h, false, [h, frame](Impl& d) { return d.do_place(h, frame); });
}

Completion<bool> ShellBackend::set_visible(WindowId id, bool visible) {
    HWND h = impl_->hwnd_of(id);
    if (!h) return completed(false);
    return impl_->run_for<bool>(h, false, [h, visible](Impl& d) { return d.do_set_visible(h, visible); });
}

Completion<FocusResult> ShellBackend::focus(WindowId id) {
    HWND h = nullptr;
    if (id == kNoWindow) {
        h = impl_->config.idle_focus_window ? to_hwnd(impl_->config.idle_focus_window) : GetShellWindow();
    } else {
        h = impl_->hwnd_of(id);
    }
    if (!h) return completed(FocusResult::NoSuchWindow);
    uint64_t gen = ++impl_->focus_generation;
    return impl_->run_for<FocusResult>(h, FocusResult::Denied,
                                       [h, gen](Impl& d) { return d.do_focus(h, gen); });
}

Completion<bool> ShellBackend::close(WindowId id) {
    HWND h = impl_->hwnd_of(id);
    if (!h) return completed(false);
    // PostMessage never blocks; no worker needed.
    return completed(impl_->do_close(h));
}

Completion<size_t> ShellBackend::restore_all() {
    // One job per tracked window, queued behind whatever is already queued
    // for its process, so a hide still in flight is undone too. The job
    // looks at the window's state when it runs.
    std::vector<HWND> tracked;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& [h, t] : impl_->by_hwnd) tracked.push_back(h);
    }
    std::vector<Completion<bool>> results;
    for (HWND h : tracked)
        results.push_back(impl_->run_for<bool>(h, false, [h](Impl& d) {
            bool hidden = false;
            {
                std::lock_guard<std::mutex> lock(d.mutex);
                auto it = d.by_hwnd.find(h);
                hidden = it != d.by_hwnd.end() && it->second.hidden != Hidden::None;
            }
            return hidden && d.do_set_visible(h, true);
        }));
    if (results.empty()) return completed(size_t(0));
    // Aggregate without blocking the caller (std::async's future would block
    // in its destructor).
    auto promise = std::make_shared<std::promise<size_t>>();
    auto f = promise->get_future().share();
    std::thread([results = std::move(results), promise] {
        size_t ok = 0;
        for (auto& r : results) ok += r.get() ? 1 : 0;
        promise->set_value(ok);
    }).detach();
    return f;
}

Completion<RecoveryReport> ShellBackend::recovery() const { return impl_->recovery; }

Completion<size_t> ShellBackend::rescue_offscreen_windows() {
    auto promise = std::make_shared<std::promise<size_t>>();
    auto f = promise->get_future().share();
    if (!impl_->workers->post(0, [self = impl_, promise] { promise->set_value(self->rescue_offscreen()); }))
        promise->set_value(0);
    return f;
}

std::optional<WindowSnapshot> ShellBackend::query(WindowId id) const {
    HWND h = impl_->hwnd_of(id);
    if (!h || !IsWindow(h)) return std::nullopt;
    DpiScope dpi;
    return impl_->make_snapshot(h);
}

std::vector<MonitorSnapshot> ShellBackend::monitors() const {
    DpiScope dpi;
    return impl_->monitors.enumerate();
}

uint64_t ShellBackend::native_handle(WindowId id) const { return from_hwnd(impl_->hwnd_of(id)); }

WindowId ShellBackend::find(uint64_t native) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->by_hwnd.find(to_hwnd(native));
    return it == impl_->by_hwnd.end() ? kNoWindow : it->second.id;
}

ReservationId ShellBackend::reserve_edge(MonitorId monitor, Edge edge, int32_t thickness) {
    if (thickness <= 0) return kNoReservation;
    {
        DpiScope dpi;
        impl_->monitors.enumerate();
    }
    if (!impl_->monitors.find(monitor)) return kNoReservation;
    ReservationId id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        id = impl_->next_reservation++;
        AppBar bar;
        bar.id = id;
        bar.monitor = monitor;
        bar.edge = edge;
        bar.thickness = thickness;
        impl_->appbars.emplace(id, bar);
    }
    if (!impl_->post([d = impl_.get(), id] { d->negotiate_new(id); })) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->appbars.erase(id);
        return kNoReservation;
    }
    return id;
}

bool ShellBackend::release_edge(ReservationId id) {
    AppBar bar;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->appbars.find(id);
        if (it == impl_->appbars.end()) return false;
        bar = it->second;
        impl_->appbars.erase(it);
    }
    // Not negotiated yet: negotiate_new() finds the record gone and backs
    // out on its own. Otherwise the shell thread removes the appbar.
    if (bar.hwnd) impl_->post([d = impl_.get(), h = bar.hwnd] { d->remove_appbar_window(h); });
    return true;
}

std::optional<Rect> ShellBackend::reservation_rect(ReservationId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->appbars.find(id);
    if (it == impl_->appbars.end() || !it->second.negotiated) return std::nullopt;
    return it->second.granted;
}

}  // namespace brocompositor::win
