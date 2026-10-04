#include "win_appbar.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace brocompositor {

namespace {

UINT to_win_edge(Edge edge) {
    switch (edge) {
        case Edge::Left:   return ABE_LEFT;
        case Edge::Top:    return ABE_TOP;
        case Edge::Right:  return ABE_RIGHT;
        case Edge::Bottom: return ABE_BOTTOM;
        default:           return ABE_TOP;
    }
}

} // namespace

WinAppBar::WinAppBar() = default;

WinAppBar::~WinAppBar() {
    for (auto& pair : registered_bars_) {
        APPBARDATA abd;
        ZeroMemory(&abd, sizeof(APPBARDATA));
        abd.cbSize = sizeof(APPBARDATA);
        abd.hWnd = reinterpret_cast<HWND>(pair.second.hwnd);
        SHAppBarMessage(ABM_REMOVE, &abd);
    }
    registered_bars_.clear();
}

void WinAppBar::set_callback(WinAppBarCallback cb) {
    callback_ = std::move(cb);
}

bool WinAppBar::register_bar(AppBarId id, void* hwnd, Edge edge, int32_t thickness) {
    HWND h = reinterpret_cast<HWND>(hwnd);

    NativeBarRecord rec;
    rec.id = id;
    rec.hwnd = hwnd;
    rec.edge = edge;
    rec.thickness = thickness;

    if (h && IsWindow(h)) {
        APPBARDATA abd;
        ZeroMemory(&abd, sizeof(APPBARDATA));
        abd.cbSize = sizeof(APPBARDATA);
        abd.hWnd = h;
        abd.uCallbackMessage = WM_USER + 101;

        if (!SHAppBarMessage(ABM_NEW, &abd)) {
            return false;
        }
    }

    registered_bars_[id] = rec;
    return update_pos(id);
}

bool WinAppBar::unregister_bar(AppBarId id) {
    auto it = registered_bars_.find(id);
    if (it == registered_bars_.end()) {
        return false;
    }

    HWND h = reinterpret_cast<HWND>(it->second.hwnd);
    if (h && IsWindow(h)) {
        APPBARDATA abd;
        ZeroMemory(&abd, sizeof(APPBARDATA));
        abd.cbSize = sizeof(APPBARDATA);
        abd.hWnd = h;
        SHAppBarMessage(ABM_REMOVE, &abd);
    }

    registered_bars_.erase(it);

    if (callback_) {
        callback_(id, get_system_work_area());
    }
    return true;
}

bool WinAppBar::update_pos(AppBarId id) {
    auto it = registered_bars_.find(id);
    if (it == registered_bars_.end()) {
        return false;
    }

    HWND h = reinterpret_cast<HWND>(it->second.hwnd);
    UINT win_edge = to_win_edge(it->second.edge);

    // Get monitor or screen rect
    RECT screen_rect;
    screen_rect.left = 0;
    screen_rect.top = 0;
    screen_rect.right = GetSystemMetrics(SM_CXSCREEN);
    screen_rect.bottom = GetSystemMetrics(SM_CYSCREEN);

    if (h && IsWindow(h)) {
        HMONITOR hMon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoW(hMon, &mi)) {
            screen_rect = mi.rcMonitor;
        }

        APPBARDATA abd;
        ZeroMemory(&abd, sizeof(APPBARDATA));
        abd.cbSize = sizeof(APPBARDATA);
        abd.hWnd = h;
        abd.uEdge = win_edge;
        abd.rc = screen_rect;

        // Propose edge rect
        switch (it->second.edge) {
            case Edge::Top:
                abd.rc.bottom = abd.rc.top + it->second.thickness;
                break;
            case Edge::Bottom:
                abd.rc.top = abd.rc.bottom - it->second.thickness;
                break;
            case Edge::Left:
                abd.rc.right = abd.rc.left + it->second.thickness;
                break;
            case Edge::Right:
                abd.rc.left = abd.rc.right - it->second.thickness;
                break;
        }

        // Query & Set
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        switch (it->second.edge) {
            case Edge::Top:
                abd.rc.bottom = abd.rc.top + it->second.thickness;
                break;
            case Edge::Bottom:
                abd.rc.top = abd.rc.bottom - it->second.thickness;
                break;
            case Edge::Left:
                abd.rc.right = abd.rc.left + it->second.thickness;
                break;
            case Edge::Right:
                abd.rc.left = abd.rc.right - it->second.thickness;
                break;
        }
        SHAppBarMessage(ABM_SETPOS, &abd);

        MoveWindow(h, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left, abd.rc.bottom - abd.rc.top, TRUE);

        it->second.allocated_rect = Rect{
            abd.rc.left,
            abd.rc.top,
            abd.rc.right - abd.rc.left,
            abd.rc.bottom - abd.rc.top
        };
    } else {
        // Mock / headless rect computation
        switch (it->second.edge) {
            case Edge::Top:
                it->second.allocated_rect = Rect{screen_rect.left, screen_rect.top, screen_rect.right - screen_rect.left, it->second.thickness};
                break;
            case Edge::Bottom:
                it->second.allocated_rect = Rect{screen_rect.left, screen_rect.bottom - it->second.thickness, screen_rect.right - screen_rect.left, it->second.thickness};
                break;
            case Edge::Left:
                it->second.allocated_rect = Rect{screen_rect.left, screen_rect.top, it->second.thickness, screen_rect.bottom - screen_rect.top};
                break;
            case Edge::Right:
                it->second.allocated_rect = Rect{screen_rect.right - it->second.thickness, screen_rect.top, it->second.thickness, screen_rect.bottom - screen_rect.top};
                break;
        }
    }

    if (callback_) {
        callback_(id, get_system_work_area());
    }
    return true;
}

Rect WinAppBar::get_system_work_area() {
    RECT r;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0)) {
        return Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
    }
    return Rect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

bool WinAppBar::set_system_work_area(const Rect& area) {
    RECT r{area.left(), area.top(), area.right(), area.bottom()};
    return SystemParametersInfoW(SPI_SETWORKAREA, 0, &r, SPIF_SENDCHANGE) != FALSE;
}

void WinAppBar::on_appbar_notification(AppBarId id, uint32_t notification) {
    if (notification == ABN_POSCHANGED) {
        update_pos(id);
    }
}

} // namespace brocompositor

#else

namespace brocompositor {

WinAppBar::WinAppBar() = default;
WinAppBar::~WinAppBar() = default;
bool WinAppBar::register_bar(AppBarId, void*, Edge, int32_t) { return false; }
bool WinAppBar::unregister_bar(AppBarId) { return false; }
bool WinAppBar::update_pos(AppBarId) { return false; }
Rect WinAppBar::get_system_work_area() { return {0, 0, 1920, 1080}; }
bool WinAppBar::set_system_work_area(const Rect&) { return false; }
void WinAppBar::set_callback(WinAppBarCallback) {}
void WinAppBar::on_appbar_notification(AppBarId, uint32_t) {}

} // namespace brocompositor

#endif
