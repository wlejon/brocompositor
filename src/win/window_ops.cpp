// Operations on foreign top-level windows. Each runs on the op worker of the
// window's process (per-monitor-v2 DPI context), never on a host thread.
// Calls that synchronously send messages to the target (SetWindowPos,
// ShowWindow) switch to their async forms once Windows reports the target
// hung; a target that hangs mid-call stalls only its own worker.
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

Rect parked_rect(const Rect& frame) {
    // Beyond the right edge of the virtual screen: no monitor can show any
    // part of it, yet DWM keeps composing it.
    Rect vs = virtual_screen();
    return Rect{vs.right() + 64, vs.y, frame.width, frame.height};
}

}  // namespace

HWND ShellBackend::Impl::hwnd_of(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : it->second;
}

bool ShellBackend::Impl::do_place(HWND h, const Rect& frame) {
    if (!IsWindow(h)) return false;
    Hidden was;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(h);
        if (it == by_hwnd.end()) return false;
        was = it->second.hidden;
        it->second.hidden = Hidden::None;  // an explicit placement also un-hides
    }
    if (was != Hidden::None) journal_sync();
    if (was == Hidden::Hide) show(h, SW_SHOWNA);
    // SW_SHOWNOACTIVATE restores a minimized or maximized window to its
    // normal state without activating it; placement then applies.
    if (IsIconic(h) || IsZoomed(h)) show(h, SW_SHOWNOACTIVATE);
    bool ok = set_frame(h, frame);
    request_report(h);
    return ok;
}

bool ShellBackend::Impl::do_set_visible(HWND h, bool visible) {
    if (!IsWindow(h)) return false;
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
            t.parked_frame = parked_rect(frame);
        }
        // Journal first: a kill between here and the move is recoverable.
        journal_sync();
        switch (method) {
            case HideMethod::Park: return move_outer(h, parked_rect(frame), SWP_NOSIZE);
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
    }
    bool ok = true;
    switch (was) {
        case Hidden::None: break;
        case Hidden::Park: ok = set_frame(h, restore); break;
        case Hidden::Minimize: show(h, SW_SHOWNOACTIVATE); break;
        case Hidden::Hide: show(h, SW_SHOWNA); break;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(h);
        if (it != by_hwnd.end()) it->second.hidden = Hidden::None;
    }
    // Only now (the window is back) does the journal forget it.
    if (was != Hidden::None) journal_sync();
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
// A newer focus request (generation) abandons this one between steps.
FocusResult ShellBackend::Impl::do_focus(HWND h, uint64_t generation) {
    auto superseded = [&] { return focus_generation.load() != generation; };
    if (!h || !IsWindow(h)) return FocusResult::NoSuchWindow;
    if (superseded()) return FocusResult::Superseded;
    if (GetForegroundWindow() == h) return FocusResult::AlreadyFocused;
    if (!input_desktop_is_ours()) return FocusResult::Denied;
    if (IsIconic(h)) show(h, SW_RESTORE);

    if (superseded()) return FocusResult::Superseded;
    if (SetForegroundWindow(h) && poll_foreground(h)) return FocusResult::Focused;

    if (superseded()) return FocusResult::Superseded;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;  // dx = dy = 0: the cursor does not move
    SendInput(1, &in, sizeof(in));
    if (SetForegroundWindow(h) && poll_foreground(h)) return FocusResult::Focused;

    if (superseded()) return FocusResult::Superseded;
    HWND fg = GetForegroundWindow();
    DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD self = GetCurrentThreadId();
    if (fg_thread && fg_thread != self && AttachThreadInput(self, fg_thread, TRUE)) {
        if (!hung(h)) BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(self, fg_thread, FALSE);
        if (poll_foreground(h)) return FocusResult::Focused;
    }
    return superseded() ? FocusResult::Superseded : FocusResult::Denied;
}

bool ShellBackend::Impl::do_close(HWND h) { return PostMessageW(h, WM_CLOSE, 0, 0) != FALSE; }

}  // namespace brocompositor::win
