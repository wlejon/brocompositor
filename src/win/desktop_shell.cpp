// The desktop-shell role: a host that replaces Explorer as the session's
// shell (a Winlogon shell) registers its own window as the shell window and
// keeps it beneath the applications' windows; and the shell hook, which every
// backend listens to.
//
// The pinning lives in a subclass of the host's window, so it runs on that
// window's thread (SetWindowSubclass works only there). Its state is process
// wide and outlives any one ShellBackend: a process has one shell window, and
// the subclass may still see messages after the backend is gone.
#include "win/shell_impl.h"
#include "win/util.h"

#include <commctrl.h>

namespace brocompositor::win {

namespace {

constexpr UINT_PTR kSubclassId = 0x62636473;  // 'bcds'

std::atomic<HWND> g_shell_window{nullptr};
std::atomic<bool> g_raised{false};

using SetWindowFn = BOOL(WINAPI*)(HWND);

SetWindowFn user32_proc(const char* name) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetWindowFn>(reinterpret_cast<void*>(GetProcAddress(user32, name))) : nullptr;
}

void pin_to_bottom(HWND h) {
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK shell_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    const bool raised = g_raised.load();
    switch (msg) {
        case WM_MOUSEACTIVATE:
            // A click on the shell (its bar) leaves the keyboard where it is.
            if (!raised) return MA_NOACTIVATE;
            break;
        case WM_WINDOWPOSCHANGING:
            if (!raised) {
                auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
                if (pos && !(pos->flags & SWP_NOZORDER)) pos->hwndInsertAfter = HWND_BOTTOM;
            }
            break;
        case WM_ACTIVATE:
            if (!raised && LOWORD(wp) != WA_INACTIVE) pin_to_bottom(h);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, shell_subclass, id);
            g_shell_window.compare_exchange_strong(h, nullptr);
            g_raised = false;
            break;
        default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------- shell hook

void ShellBackend::Impl::listen_shell_hook() {
    UINT msg = RegisterWindowMessageW(L"SHELLHOOK");
    if (msg && RegisterShellHookWindow(listener)) shell_hook_message = msg;
}

void ShellBackend::Impl::unlisten_shell_hook() {
    if (!shell_hook_message) return;
    DeregisterShellHookWindow(listener);
    shell_hook_message = 0;
}

void ShellBackend::Impl::on_shell_hook(WPARAM code, LPARAM lp) {
    // HSHELL_GETMINRECT carries a SHELLHOOKINFO pointer of another process;
    // every code used here carries the window itself.
    HWND h = reinterpret_cast<HWND>(lp);
    if (!h || h == listener) return;
    auto tracked_id = [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(h);
        return it == by_hwnd.end() ? kNoWindow : it->second.id;
    };
    // The shell hook feeds the one tracking path the WinEvent hooks feed, so
    // the two can never disagree about which windows exist.
    switch (code) {
        case HSHELL_WINDOWCREATED:
            consider(h);
            break;
        case HSHELL_WINDOWDESTROYED:
            on_win_event(EVENT_OBJECT_DESTROY, h, OBJID_WINDOW, CHILDID_SELF);
            break;
        case HSHELL_REDRAW:
            if (tracked_id() != kNoWindow) mark_dirty(h);
            break;
        case HSHELL_FLASH: {
            WindowId id = tracked_id();
            if (id == kNoWindow) {
                consider(h);
                id = tracked_id();
            }
            if (id == kNoWindow) break;
            flush_one(h);
            queue.push(WindowAttention{id});
            break;
        }
        default: break;  // activation: EVENT_SYSTEM_FOREGROUND reports focus
    }
}

// ---------------------------------------------------------------- shell window

bool ShellBackend::attach_desktop_shell(uint64_t native_window) {
    HWND h = to_hwnd(native_window);
    if (!h || !IsWindow(h)) return false;
    DWORD pid = 0;
    if (GetWindowThreadProcessId(h, &pid) != GetCurrentThreadId() || pid != GetCurrentProcessId()) return false;
    HWND expected = nullptr;
    if (!g_shell_window.compare_exchange_strong(expected, h)) return false;
    // Neither is in the SDK headers; both are what Explorer calls. The
    // task-manager window only follows a granted shell window: with another
    // shell running it is that shell's, and taking it would break it.
    auto set_shell = user32_proc("SetShellWindow");
    if (set_shell && set_shell(h) && GetShellWindow() == h) {
        if (auto set_taskman = user32_proc("SetTaskmanWindow")) set_taskman(h);
    }
    g_raised = false;
    if (!SetWindowSubclass(h, shell_subclass, kSubclassId, 0)) {
        g_shell_window = nullptr;
        return false;
    }
    pin_to_bottom(h);
    return true;
}

bool ShellBackend::is_desktop_shell() const {
    HWND h = g_shell_window.load();
    return h && GetShellWindow() == h;
}

bool ShellBackend::set_shell_raised(bool raised) {
    HWND h = g_shell_window.load();
    if (!h) return false;
    g_raised = raised;
    if (raised) {
        // Over the applications, with the keyboard.
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(h);
    } else {
        // Back beneath them; whichever application the user picks next may
        // take the foreground from the shell.
        pin_to_bottom(h);
        AllowSetForegroundWindow(ASFW_ANY);
    }
    return true;
}

}  // namespace brocompositor::win
