#include "win/util.h"

#include <dwmapi.h>

namespace brocompositor::win {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

DpiScope::DpiScope()
    : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

DpiScope::~DpiScope() {
    if (previous_) SetThreadDpiAwarenessContext(previous_);
}

Rect frame_bounds(HWND hwnd) {
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) &&
        r.right > r.left)
        return to_rect(r);
    GetWindowRect(hwnd, &r);
    return to_rect(r);
}

Margins invisible_borders(HWND hwnd) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return {};
    Rect f = frame_bounds(hwnd);
    return Margins{std::max(0, int32_t(f.x - wr.left)), std::max(0, int32_t(f.y - wr.top)),
                   std::max(0, int32_t(wr.right - f.right())),
                   std::max(0, int32_t(wr.bottom - f.bottom()))};
}

bool is_cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

std::wstring window_class(HWND hwnd) {
    wchar_t buf[256];
    int n = GetClassNameW(hwnd, buf, 256);
    return std::wstring(buf, size_t(std::max(0, n)));
}

std::wstring window_title(HWND hwnd) {
    // GetWindowTextW does not send WM_GETTEXT to windows of other processes,
    // so a hung application cannot block us here.
    wchar_t buf[512];
    int n = GetWindowTextW(hwnd, buf, 512);
    return std::wstring(buf, size_t(std::max(0, n)));
}

std::string process_image_name(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring path;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) path.assign(buf, n);
    CloseHandle(p);
    size_t slash = path.find_last_of(L"\\/");
    return to_utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

HINSTANCE module_instance() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_instance), &m);
    return m;
}

Rect virtual_screen() {
    return Rect{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

}  // namespace brocompositor::win
