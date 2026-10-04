#include "win/shell_impl.h"
#include "win/util.h"

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
    // receive WM_DISPLAYCHANGE / WM_SETTINGCHANGE broadcasts.
    d->listener = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"brocompositor.listener", L"",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module_instance(), nullptr);
    if (!d->listener) {
        ready->set_value("CreateWindowEx(listener) failed: " + std::to_string(GetLastError()));
        return;
    }
    SetWindowLongPtrW(d->listener, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    if (!d->install_hooks()) {
        DestroyWindow(d->listener);
        ready->set_value("SetWinEventHook failed");
        return;
    }
    d->report_initial_state();
    ready->set_value(std::string());
    ready = nullptr;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    d->remove_hooks();
    DestroyWindow(d->listener);
    d->listener = nullptr;
}

}  // namespace

LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<ShellBackend::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (d) {
        switch (msg) {
            case kMsgCall:
                (*reinterpret_cast<const std::function<void()>*>(lp))();
                return 0;
            case kMsgFlush:
                d->flush();
                return 0;
            case kMsgDirty:
                d->mark_dirty(reinterpret_cast<HWND>(lp));
                return 0;
            case WM_DISPLAYCHANGE:
            case WM_DPICHANGED:
                d->report_monitors_if_changed();
                break;
            case WM_SETTINGCHANGE:
                if (wp == SPI_SETWORKAREA) d->report_monitors_if_changed();
                break;
            default: break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShellBackend::Impl::call(const std::function<void()>& fn) {
    if (on_shell_thread()) {
        fn();
        return;
    }
    SendMessageW(listener, kMsgCall, 0, reinterpret_cast<LPARAM>(&fn));
}

bool ShellBackend::Impl::in_scope(HWND hwnd) const {
    if (config.process_filter.empty()) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (uint32_t p : config.process_filter)
        if (p == pid) return true;
    return false;
}

// ---------------------------------------------------------------- public

ShellBackend::ShellBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<ShellBackend> ShellBackend::create(const ShellConfig& config, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    std::promise<std::string> ready;
    auto result = ready.get_future();
    Impl* d = impl.get();
    impl->thread = std::thread([d, &ready] { shell_thread(d, &ready); });
    impl->thread_id = GetThreadId(impl->thread.native_handle());
    std::string err = result.get();
    if (!err.empty()) {
        impl->thread.join();
        if (error) *error = err;
        return nullptr;
    }
    return std::unique_ptr<ShellBackend>(new ShellBackend(std::move(impl)));
}

ShellBackend::~ShellBackend() {
    impl_->restore_all();
    impl_->call([this] { impl_->release_all(); });
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
}

EventQueue& ShellBackend::events() { return impl_->queue; }

bool ShellBackend::execute(const Command& command) {
    if (auto* c = std::get_if<PlaceWindow>(&command)) return place(c->id, c->frame);
    if (auto* c = std::get_if<SetWindowVisible>(&command)) return set_visible(c->id, c->visible);
    if (auto* c = std::get_if<FocusWindow>(&command)) {
        FocusResult r = focus(c->id);
        return r == FocusResult::Focused || r == FocusResult::AlreadyFocused;
    }
    if (auto* c = std::get_if<CloseWindow>(&command)) return close(c->id);
    return false;
}

size_t ShellBackend::execute(const std::vector<Command>& commands) {
    size_t failures = 0;
    for (const auto& c : commands) failures += execute(c) ? 0 : 1;
    return failures;
}

bool ShellBackend::place(WindowId id, const Rect& frame) { return impl_->place(id, frame); }
bool ShellBackend::set_visible(WindowId id, bool visible) { return impl_->set_visible(id, visible); }
FocusResult ShellBackend::focus(WindowId id) { return impl_->focus(id); }
bool ShellBackend::close(WindowId id) { return impl_->close(id); }
void ShellBackend::restore_all() { impl_->restore_all(); }
size_t ShellBackend::rescue_offscreen_windows() { return impl_->rescue_offscreen(); }

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

ReservationId ShellBackend::reserve_edge(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted) {
    return impl_->reserve(monitor, edge, thickness, granted);
}

bool ShellBackend::release_edge(ReservationId id) { return impl_->release(id); }

std::optional<Rect> ShellBackend::reservation_rect(ReservationId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->appbars.find(id);
    if (it == impl_->appbars.end()) return std::nullopt;
    return it->second.granted;
}

}  // namespace brocompositor::win
