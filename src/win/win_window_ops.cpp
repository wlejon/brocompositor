#include "win_window_ops.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace brocompositor {

WindowId WinWindowOps::hwnd_to_id(void* hwnd) {
    return reinterpret_cast<WindowId>(hwnd);
}

void* WinWindowOps::id_to_hwnd(WindowId id) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

bool WinWindowOps::set_window_rect(WindowId id, const Rect& rect) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    // Restore if minimized or maximized
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
    return SetWindowPos(hwnd, nullptr, rect.x, rect.y, rect.width, rect.height, flags) != FALSE;
}

bool WinWindowOps::set_window_state(WindowId id, WindowState state) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    if (has_state(state, WindowState::Minimized)) {
        ShowWindow(hwnd, SW_MINIMIZE);
    } else if (has_state(state, WindowState::Maximized)) {
        ShowWindow(hwnd, SW_MAXIMIZE);
    } else if (has_state(state, WindowState::Hidden)) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_RESTORE);
    }

    if (has_state(state, WindowState::Focused)) {
        focus_window(id);
    }

    return true;
}

bool WinWindowOps::focus_window(WindowId id) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    BringWindowToTop(hwnd);
    return SetForegroundWindow(hwnd) != FALSE;
}

bool WinWindowOps::close_window(WindowId id) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    return PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
}

bool WinWindowOps::get_window_info(WindowId id, WindowInfo& out_info) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    out_info.id = id;

    // Title
    wchar_t title_buf[512] = {0};
    GetWindowTextW(hwnd, title_buf, 512);
    char title_utf8[1024] = {0};
    WideCharToMultiByte(CP_UTF8, 0, title_buf, -1, title_utf8, sizeof(title_utf8), nullptr, nullptr);
    out_info.title = title_utf8;

    // Class name
    wchar_t class_buf[256] = {0};
    GetClassNameW(hwnd, class_buf, 256);
    char class_utf8[512] = {0};
    WideCharToMultiByte(CP_UTF8, 0, class_buf, -1, class_utf8, sizeof(class_utf8), nullptr, nullptr);
    out_info.class_name = class_utf8;

    // Process ID
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    out_info.process_id = static_cast<uint32_t>(pid);

    // App ID / process name
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t proc_path[MAX_PATH] = {0};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, proc_path, &sz)) {
            wchar_t* fname = wcsrchr(proc_path, L'\\');
            if (fname) fname++;
            else fname = proc_path;
            char proc_utf8[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, fname, -1, proc_utf8, sizeof(proc_utf8), nullptr, nullptr);
            out_info.app_id = proc_utf8;
        }
        CloseHandle(proc);
    }

    // Geometry
    RECT r;
    if (GetWindowRect(hwnd, &r)) {
        out_info.geometry = Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
    }

    // State
    WindowState state = WindowState::None;
    if (IsIconic(hwnd)) {
        state |= WindowState::Minimized;
    } else if (IsZoomed(hwnd)) {
        state |= WindowState::Maximized;
    }
    if (!IsWindowVisible(hwnd)) {
        state |= WindowState::Hidden;
    }
    if (GetForegroundWindow() == hwnd) {
        state |= WindowState::Focused;
    }

    out_info.state = state;
    return true;
}

bool WinWindowOps::is_manageable_window(WindowId id) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    // Must be visible
    if (!IsWindowVisible(hwnd)) return false;

    // Owner check: top-level windows shouldn't have an owner (unless they have WS_EX_APPWINDOW)
    HWND owner = GetWindow(hwnd, GW_OWNER);
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    if (owner != nullptr && !(ex_style & WS_EX_APPWINDOW)) {
        return false;
    }

    // Tool windows, transparent/layered invisible overlays
    if (ex_style & WS_EX_TOOLWINDOW) {
        return false;
    }

    // Child windows
    if (style & WS_CHILD) {
        return false;
    }

    // Ignore Windows desktop, shell tray, etc.
    wchar_t class_buf[128] = {0};
    GetClassNameW(hwnd, class_buf, 128);
    if (wcscmp(class_buf, L"Progman") == 0 ||
        wcscmp(class_buf, L"WorkerW") == 0 ||
        wcscmp(class_buf, L"Shell_TrayWnd") == 0 ||
        wcscmp(class_buf, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(class_buf, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    // Cloaked UWP window check
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        if (cloaked != 0) {
            return false;
        }
    }

    // Must have non-empty geometry
    RECT r;
    if (GetWindowRect(hwnd, &r)) {
        if (r.right - r.left <= 0 || r.bottom - r.top <= 0) {
            return false;
        }
    }

    return true;
}

bool WinWindowOps::set_dark_mode(WindowId id, bool enable) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    BOOL value = enable ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
    return SUCCEEDED(hr);
}

bool WinWindowOps::set_corner_preference(WindowId id, CornerPreference pref) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    DWORD val = 0;
    switch (pref) {
        case CornerPreference::DoNotRound: val = 1; break;
        case CornerPreference::Round:      val = 2; break;
        case CornerPreference::RoundSmall: val = 3; break;
        case CornerPreference::Default:
        default: val = 0; break;
    }

    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &val, sizeof(val));
    return SUCCEEDED(hr);
}

bool WinWindowOps::set_cloaked(WindowId id, bool cloaked) {
    HWND hwnd = reinterpret_cast<HWND>(id_to_hwnd(id));
    if (!hwnd || !IsWindow(hwnd)) return false;

    // Cloaking can be toggled or fallback to ShowWindow
    int val = cloaked ? 1 : 0;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &val, sizeof(val));
    if (FAILED(hr)) {
        ShowWindow(hwnd, cloaked ? SW_HIDE : SW_SHOW);
    }
    return true;
}

} // namespace brocompositor

#else

namespace brocompositor {

WindowId WinWindowOps::hwnd_to_id(void* hwnd) { return reinterpret_cast<WindowId>(hwnd); }
void* WinWindowOps::id_to_hwnd(WindowId id) { return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); }
bool WinWindowOps::set_window_rect(WindowId, const Rect&) { return false; }
bool WinWindowOps::set_window_state(WindowId, WindowState) { return false; }
bool WinWindowOps::focus_window(WindowId) { return false; }
bool WinWindowOps::close_window(WindowId) { return false; }
bool WinWindowOps::get_window_info(WindowId, WindowInfo&) { return false; }
bool WinWindowOps::is_manageable_window(WindowId) { return false; }
bool WinWindowOps::set_dark_mode(WindowId, bool) { return false; }
bool WinWindowOps::set_corner_preference(WindowId, CornerPreference) { return false; }
bool WinWindowOps::set_cloaked(WindowId, bool) { return false; }

} // namespace brocompositor

#endif
