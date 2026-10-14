#include "win/window_query.h"

#include "win/monitors.h"
#include "win/util.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>

namespace brocompositor::win {

namespace {

bool is_shell_class(const std::wstring& cls) {
    return cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" ||
           cls == L"Shell_SecondaryTrayWnd" || cls == L"brocompositor.listener" ||
           cls == L"brocompositor.appbar";
}

std::wstring process_full_path(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring path;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) path.assign(buf, n);
    CloseHandle(p);
    return path;
}

std::optional<WindowIcon> hicon_to_window_icon(HICON hicon) {
    if (!hicon) return std::nullopt;
    ICONINFO ii{};
    if (!GetIconInfo(hicon, &ii)) return std::nullopt;
    auto cleanup_bitmaps = [&] {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    };

    int32_t w = 0;
    int32_t h = 0;
    if (ii.hbmColor) {
        BITMAP bm{};
        if (GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
            w = bm.bmWidth;
            h = bm.bmHeight;
        }
    } else if (ii.hbmMask) {
        BITMAP bm{};
        if (GetObjectW(ii.hbmMask, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 1) {
            w = bm.bmWidth;
            h = bm.bmHeight / 2;
        }
    }
    if (w <= 0 || h <= 0 || w > 512 || h > 512) {
        cleanup_bitmaps();
        return std::nullopt;
    }

    int32_t side = std::min(w, h);
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
        cleanup_bitmaps();
        return std::nullopt;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra(size_t(w) * size_t(h) * 4, 0);
    bool got_pixels = false;
    if (ii.hbmColor && GetDIBits(hdc, ii.hbmColor, 0, UINT(h), bgra.data(), &bmi, DIB_RGB_COLORS) > 0) {
        got_pixels = true;
        bool has_alpha = false;
        for (size_t i = 3; i < bgra.size(); i += 4) {
            if (bgra[i] != 0) {
                has_alpha = true;
                break;
            }
        }
        if (!has_alpha && ii.hbmMask) {
            std::vector<uint8_t> mask(size_t(w) * size_t(h) * 4, 0);
            if (GetDIBits(hdc, ii.hbmMask, 0, UINT(h), mask.data(), &bmi, DIB_RGB_COLORS) > 0) {
                for (size_t i = 0; i < bgra.size(); i += 4) {
                    bgra[i + 3] = (mask[i] == 0) ? 255 : 0;
                }
            } else {
                for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
            }
        }
    } else {
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            HGDIOBJ old = SelectObject(hdc, dib);
            DrawIconEx(hdc, 0, 0, hicon, w, h, 0, nullptr, DI_NORMAL);
            GdiFlush();
            std::memcpy(bgra.data(), bits, bgra.size());
            for (size_t i = 0; i < bgra.size(); i += 4) {
                if (bgra[i + 3] == 0 && (bgra[i] != 0 || bgra[i + 1] != 0 || bgra[i + 2] != 0))
                    bgra[i + 3] = 255;
            }
            got_pixels = true;
            SelectObject(hdc, old);
        }
        if (dib) DeleteObject(dib);
    }

    DeleteDC(hdc);
    cleanup_bitmaps();
    if (!got_pixels) return std::nullopt;

    WindowIcon icon;
    icon.size = side;
    icon.rgba.resize(size_t(side) * size_t(side) * 4);
    for (int32_t y = 0; y < side; ++y) {
        for (int32_t x = 0; x < side; ++x) {
            const uint8_t* src = &bgra[(size_t(y) * size_t(w) + size_t(x)) * 4];
            uint8_t* dst = &icon.rgba[(size_t(y) * size_t(side) + size_t(x)) * 4];
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    return icon;
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
    s.icon_serial = 1;
    return s;
}

std::optional<WindowIcon> extract_window_icon(HWND hwnd, int32_t desired_size) {
    if (!hwnd || !IsWindow(hwnd)) return std::nullopt;
    bool want_small = (desired_size > 0 && desired_size <= 24);
    HICON hicon = nullptr;
    bool owns_icon = false;

    const WPARAM order_large[] = {ICON_BIG, 2 /* ICON_SMALL2 */, ICON_SMALL};
    const WPARAM order_small[] = {2 /* ICON_SMALL2 */, ICON_SMALL, ICON_BIG};
    const WPARAM* order = want_small ? order_small : order_large;
    for (int i = 0; i < 3 && !hicon; ++i) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, order[i], 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &res) && res) {
            hicon = reinterpret_cast<HICON>(res);
        }
    }

    if (!hicon) {
        ULONG_PTR first = GetClassLongPtrW(hwnd, want_small ? GCLP_HICONSM : GCLP_HICON);
        ULONG_PTR second = GetClassLongPtrW(hwnd, want_small ? GCLP_HICON : GCLP_HICONSM);
        if (first) hicon = reinterpret_cast<HICON>(first);
        else if (second) hicon = reinterpret_cast<HICON>(second);
    }

    if (!hicon) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring exe = process_full_path(pid);
        if (!exe.empty()) {
            SHFILEINFOW sfi{};
            UINT flags = SHGFI_ICON | (want_small ? SHGFI_SMALLICON : SHGFI_LARGEICON);
            if (SHGetFileInfoW(exe.c_str(), 0, &sfi, sizeof(sfi), flags) && sfi.hIcon) {
                hicon = sfi.hIcon;
                owns_icon = true;
            }
        }
    }

    if (!hicon) {
        hicon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    auto result = hicon_to_window_icon(hicon);
    if (owns_icon && hicon) DestroyIcon(hicon);
    return result;
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
