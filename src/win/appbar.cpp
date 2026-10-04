// Edge reservations through the shell appbar protocol (SHAppBarMessage).
//
// Each reservation is a hidden top-level window on the shell thread
// registered with ABM_NEW. Its rectangle is negotiated per monitor with
// ABM_QUERYPOS / ABM_SETPOS, after which the shell shrinks that monitor's
// work area and broadcasts WM_SETTINGCHANGE(SPI_SETWORKAREA); the listener
// turns that into MonitorsChanged. ABN_POSCHANGED (taskbar moved, another bar
// registered) renegotiates and emits ReservationChanged. ABM_REMOVE gives the
// space back; every exit path (release, destructor, emergency) ends in it.
#include "win/shell_impl.h"
#include "win/util.h"

#include <shellapi.h>

namespace brocompositor::win {

namespace {

std::mutex g_emergency_mutex;
std::set<HWND> g_emergency;

UINT to_abe(Edge e) {
    switch (e) {
        case Edge::Left: return ABE_LEFT;
        case Edge::Top: return ABE_TOP;
        case Edge::Right: return ABE_RIGHT;
        case Edge::Bottom: return ABE_BOTTOM;
    }
    return ABE_TOP;
}

void appbar_remove(HWND h) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = h;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

}  // namespace

void register_emergency_appbar(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_emergency_mutex);
    g_emergency.insert(hwnd);
}

void unregister_emergency_appbar(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_emergency_mutex);
    g_emergency.erase(hwnd);
}

void emergency_release_reservations() {
    std::lock_guard<std::mutex> lock(g_emergency_mutex);
    for (HWND h : g_emergency) appbar_remove(h);
    g_emergency.clear();
}

LRESULT CALLBACK appbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgAppBar) {
        auto* d = reinterpret_cast<ShellBackend::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (d) d->on_appbar_message(hwnd, wp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShellBackend::Impl::negotiate(AppBar& bar) {
    auto mon = monitors.find(bar.monitor);
    if (!mon) return false;
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = bar.hwnd;
    abd.uEdge = to_abe(bar.edge);
    abd.rc = to_RECT(mon->bounds);
    // QUERYPOS moves the edge-side of the proposal past bars already on that
    // edge (the taskbar included); the thickness is applied from there.
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    switch (bar.edge) {
        case Edge::Top: abd.rc.bottom = abd.rc.top + bar.thickness; break;
        case Edge::Bottom: abd.rc.top = abd.rc.bottom - bar.thickness; break;
        case Edge::Left: abd.rc.right = abd.rc.left + bar.thickness; break;
        case Edge::Right: abd.rc.left = abd.rc.right - bar.thickness; break;
    }
    SHAppBarMessage(ABM_SETPOS, &abd);
    bar.granted = to_rect(abd.rc);
    // Keep the (never shown) appbar window over its rectangle so the shell's
    // own bookkeeping matches the reservation.
    SetWindowPos(bar.hwnd, nullptr, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left,
                 abd.rc.bottom - abd.rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return !bar.granted.empty();
}

ReservationId ShellBackend::Impl::reserve(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted) {
    ReservationId result = kNoReservation;
    call([&] {
        monitors.enumerate();
        if (thickness <= 0 || !monitors.find(monitor)) return;
        HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"brocompositor.appbar", L"",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module_instance(), nullptr);
        if (!h) return;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = h;
        abd.uCallbackMessage = kMsgAppBar;
        if (!SHAppBarMessage(ABM_NEW, &abd)) {
            DestroyWindow(h);
            return;
        }
        register_emergency_appbar(h);
        AppBar bar;
        bar.hwnd = h;
        bar.monitor = monitor;
        bar.edge = edge;
        bar.thickness = thickness;
        if (!negotiate(bar)) {
            appbar_remove(h);
            unregister_emergency_appbar(h);
            DestroyWindow(h);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            bar.id = next_reservation++;
            appbars.emplace(bar.id, bar);
        }
        result = bar.id;
        if (granted) *granted = bar.granted;
        report_monitors_if_changed();
    });
    return result;
}

bool ShellBackend::Impl::release(ReservationId id) {
    bool ok = false;
    call([&] {
        AppBar bar;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = appbars.find(id);
            if (it == appbars.end()) return;
            bar = it->second;
            appbars.erase(it);
        }
        appbar_remove(bar.hwnd);
        unregister_emergency_appbar(bar.hwnd);
        DestroyWindow(bar.hwnd);
        ok = true;
        report_monitors_if_changed();
    });
    return ok;
}

void ShellBackend::Impl::release_all() {
    std::vector<ReservationId> ids;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [id, bar] : appbars) ids.push_back(id);
    }
    for (ReservationId id : ids) release(id);
}

void ShellBackend::Impl::on_appbar_message(HWND hwnd, WPARAM notification) {
    if (notification != ABN_POSCHANGED) return;
    std::optional<AppBar> bar;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [id, b] : appbars)
            if (b.hwnd == hwnd) bar = b;
    }
    if (!bar) return;
    Rect before = bar->granted;
    monitors.enumerate();
    if (!negotiate(*bar)) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = appbars.find(bar->id);
        if (it != appbars.end()) it->second.granted = bar->granted;
    }
    if (bar->granted != before) queue.push(ReservationChanged{bar->id, bar->monitor, bar->granted});
}

}  // namespace brocompositor::win
