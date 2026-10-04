// Operations on foreign top-level windows. Runs on the caller's thread inside
// a per-monitor-v2 DPI scope. Calls that synchronously send messages to the
// target (SetWindowPos, ShowWindow) switch to their async forms when the
// target is hung, so a frozen application can never block the host.
#include "win/shell_impl.h"
#include "win/util.h"

namespace brocompositor::win {

namespace {

bool hung(HWND h) { return IsHungAppWindow(h) != FALSE; }

void show(HWND h, int cmd) {
    if (hung(h)) ShowWindowAsync(h, cmd);
    else ShowWindow(h, cmd);
}

bool move_outer(HWND h, const Rect& frame, UINT extra) {
    Margins b = invisible_borders(h);
    Rect o = frame.outset(b);
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | extra;
    if (hung(h)) flags |= SWP_ASYNCWINDOWPOS;
    return SetWindowPos(h, nullptr, o.x, o.y, o.width, o.height, flags) != FALSE;
}

// Places the visible frame. The invisible-border margins depend on the DPI
// of the monitor the window ends up on, and DPI-aware apps rescale on
// WM_DPICHANGED during the move, so a cross-monitor move is corrected once.
bool set_frame(HWND h, const Rect& frame) {
    if (!move_outer(h, frame, 0)) return false;
    if (!hung(h) && frame_bounds(h) != frame) move_outer(h, frame, 0);
    return true;
}

std::wstring desktop_name(HDESK d) {
    wchar_t name[128] = L"";
    DWORD n = 0;
    if (d) GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &n);
    return name;
}

// False while a screen saver, the lock screen or a UAC prompt owns input:
// no window on our desktop can take the foreground then, and injecting input
// would only disturb the user.
bool input_desktop_is_ours() {
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!input) return false;
    bool same = desktop_name(input) == desktop_name(GetThreadDesktop(GetCurrentThreadId()));
    CloseDesktop(input);
    return same;
}

bool poll_foreground(HWND h) {
    for (int i = 0; i < 20; ++i) {
        if (GetForegroundWindow() == h) return true;
        Sleep(5);
    }
    return false;
}

}  // namespace

HWND ShellBackend::Impl::hwnd_of(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : it->second;
}

bool ShellBackend::Impl::place(WindowId id, const Rect& frame) {
    DpiScope dpi;
    HWND h = hwnd_of(id);
    if (!h || !IsWindow(h) || frame.empty()) return false;
    Hidden was;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(h);
        if (it == by_hwnd.end()) return false;
        was = it->second.hidden;
        it->second.hidden = Hidden::None;  // an explicit placement also un-hides
    }
    if (was == Hidden::Hide) show(h, SW_SHOWNA);
    // SW_SHOWNOACTIVATE restores a minimized or maximized window to its
    // normal state without activating it; placement then applies.
    if (IsIconic(h) || IsZoomed(h)) show(h, SW_SHOWNOACTIVATE);
    bool ok = set_frame(h, frame);
    request_report(h);
    return ok;
}

bool ShellBackend::Impl::set_visible(WindowId id, bool visible) {
    DpiScope dpi;
    HWND h = hwnd_of(id);
    if (!h || !IsWindow(h)) return false;
    if (!visible) {
        if (IsIconic(h)) return true;  // already off screen, by the user's choice
        HideMethod method = config.hide_method;
        if (method == HideMethod::Park && IsZoomed(h)) method = HideMethod::Minimize;
        Rect frame = frame_bounds(h);
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_hwnd.find(h);
            if (it == by_hwnd.end()) return false;
            Tracked& t = it->second;
            if (t.hidden != Hidden::None) return true;
            t.hidden = method == HideMethod::Park       ? Hidden::Park
                       : method == HideMethod::Minimize ? Hidden::Minimize
                                                        : Hidden::Hide;
            t.restore_frame = frame;
        }
        switch (method) {
            case HideMethod::Park: {
                // Beyond the right edge of the virtual screen: no monitor can
                // show any part of it, yet DWM keeps composing it.
                Rect vs = virtual_screen();
                return move_outer(h, Rect{vs.right() + 64, vs.y, frame.width, frame.height}, SWP_NOSIZE);
            }
            case HideMethod::Minimize: show(h, SW_SHOWMINNOACTIVE); return true;
            case HideMethod::Hide: show(h, SW_HIDE); return true;
        }
        return false;
    }
    Hidden was;
    Rect restore;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(h);
        if (it == by_hwnd.end()) return false;
        was = it->second.hidden;
        restore = it->second.restore_frame;
        it->second.hidden = Hidden::None;
    }
    bool ok = true;
    switch (was) {
        case Hidden::None: break;
        case Hidden::Park: ok = set_frame(h, restore); break;
        case Hidden::Minimize: show(h, SW_SHOWNOACTIVATE); break;
        case Hidden::Hide: show(h, SW_SHOWNA); break;
    }
    request_report(h);
    return ok;
}

// Windows only lets a process take the foreground under conditions (it is
// the foreground process, it received the last input event, the lock timeout
// expired, ...). Strategies, in order of politeness:
//   1. plain SetForegroundWindow (works when we already hold the foreground);
//   2. inject a zero-length mouse move, which makes this process the sender of
//      the last input event, then retry;
//   3. attach to the foreground thread's input queue and retry.
FocusResult ShellBackend::Impl::focus_hwnd(HWND h) {
    if (!h || !IsWindow(h)) return FocusResult::NoSuchWindow;
    if (GetForegroundWindow() == h) return FocusResult::AlreadyFocused;
    if (!input_desktop_is_ours()) return FocusResult::Denied;
    if (IsIconic(h)) show(h, SW_RESTORE);

    if (SetForegroundWindow(h) && poll_foreground(h)) return FocusResult::Focused;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;  // dx = dy = 0: the cursor does not move
    SendInput(1, &in, sizeof(in));
    if (SetForegroundWindow(h) && poll_foreground(h)) return FocusResult::Focused;

    HWND fg = GetForegroundWindow();
    DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD self = GetCurrentThreadId();
    if (fg_thread && fg_thread != self && AttachThreadInput(self, fg_thread, TRUE)) {
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(self, fg_thread, FALSE);
        if (poll_foreground(h)) return FocusResult::Focused;
    }
    return FocusResult::Denied;
}

FocusResult ShellBackend::Impl::focus(WindowId id) {
    if (id == kNoWindow) {
        HWND idle = config.idle_focus_window ? to_hwnd(config.idle_focus_window) : GetShellWindow();
        return focus_hwnd(idle);
    }
    HWND h = hwnd_of(id);
    if (!h) return FocusResult::NoSuchWindow;
    return focus_hwnd(h);
}

bool ShellBackend::Impl::close(WindowId id) {
    HWND h = hwnd_of(id);
    return h && PostMessageW(h, WM_CLOSE, 0, 0) != FALSE;
}

void ShellBackend::Impl::restore_all() {
    std::vector<WindowId> hidden;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [h, t] : by_hwnd)
            if (t.hidden != Hidden::None) hidden.push_back(t.id);
    }
    for (WindowId id : hidden) set_visible(id, true);
}

size_t ShellBackend::Impl::rescue_offscreen() {
    DpiScope dpi;
    Rect vs = virtual_screen();
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    Rect work = to_rect(mi.rcWork);
    std::vector<HWND> all;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&all));
    size_t moved = 0;
    for (HWND h : all) {
        if (!in_scope(h) || !IsWindowVisible(h) || IsIconic(h) || h == listener) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        Rect f = frame_bounds(h);
        if (f.empty() || f.intersects(vs)) continue;
        Rect target{work.x + 64, work.y + 64, std::min(f.width, work.width - 128),
                    std::min(f.height, work.height - 128)};
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_hwnd.find(h);
            if (it != by_hwnd.end()) it->second.hidden = Hidden::None;
        }
        if (set_frame(h, target)) ++moved;
        request_report(h);
    }
    return moved;
}

}  // namespace brocompositor::win
