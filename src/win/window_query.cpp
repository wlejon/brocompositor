#include "win/window_query.h"

#include "win/monitors.h"
#include "win/util.h"

namespace brocompositor::win {

namespace {

bool is_shell_class(const std::wstring& cls) {
    return cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" ||
           cls == L"Shell_SecondaryTrayWnd" || cls == L"brocompositor.listener" ||
           cls == L"brocompositor.appbar";
}

}  // namespace

bool is_manageable(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) return false;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD) return false;
    bool app = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !app) return false;
    if ((ex & WS_EX_NOACTIVATE) && !app) return false;
    // Owned windows (dialogs, palettes) are managed only when they look like
    // windows a user can address: captioned or explicitly app windows.
    if (GetWindow(hwnd, GW_OWNER) && !app && (style & WS_CAPTION) != WS_CAPTION) return false;
    if (is_cloaked(hwnd)) return false;
    if (is_shell_class(window_class(hwnd))) return false;
    return !frame_bounds(hwnd).empty();
}

WindowSnapshot snapshot_window(HWND hwnd, MonitorRegistry& monitors) {
    WindowSnapshot s;
    s.native = from_hwnd(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    s.process_id = pid;
    s.title = to_utf8(window_title(hwnd));
    s.class_name = to_utf8(window_class(hwnd));
    s.frame = frame_bounds(hwnd);
    s.minimized = IsIconic(hwnd) != FALSE;
    s.maximized = IsZoomed(hwnd) != FALSE;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    s.resizable = (style & WS_THICKFRAME) != 0;
    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    s.monitor = monitors.id_for(hm);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi)) {
        Rect mb = to_rect(mi.rcMonitor);
        s.fullscreen = !s.minimized && !s.maximized && s.frame.contains(mb);
    }
    if (auto m = monitors.find(s.monitor)) s.dpi = m->dpi;
    return s;
}

uint32_t diff(const WindowSnapshot& a, const WindowSnapshot& b) {
    uint32_t c = 0;
    if (a.frame != b.frame) c |= change::Geometry;
    if (a.title != b.title) c |= change::Title;
    if (a.minimized != b.minimized || a.maximized != b.maximized || a.fullscreen != b.fullscreen)
        c |= change::State;
    if (a.monitor != b.monitor || a.dpi != b.dpi) c |= change::Monitor;
    return c;
}

}  // namespace brocompositor::win
