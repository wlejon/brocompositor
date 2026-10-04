// Small Win32 helpers shared by the Windows backend.
#pragma once

#include "brocompositor/geometry.h"

#include <windows.h>

#include <string>

namespace brocompositor::win {

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);

inline Rect to_rect(const RECT& r) { return Rect{r.left, r.top, r.right - r.left, r.bottom - r.top}; }
inline RECT to_RECT(const Rect& r) { return RECT{r.x, r.y, r.right(), r.bottom()}; }

inline HWND to_hwnd(uint64_t v) { return reinterpret_cast<HWND>(static_cast<uintptr_t>(v)); }
inline uint64_t from_hwnd(HWND h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

// Runs the enclosed scope with a per-monitor-v2 DPI context so window and
// monitor coordinates are physical pixels.
class DpiScope {
public:
    DpiScope();
    ~DpiScope();
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

// The visible frame (DWMWA_EXTENDED_FRAME_BOUNDS): excludes the invisible
// resize borders that GetWindowRect includes on Windows 10/11.
Rect frame_bounds(HWND hwnd);
// GetWindowRect minus frame_bounds, per edge (>= 0).
Margins invisible_borders(HWND hwnd);
bool is_cloaked(HWND hwnd);
std::wstring window_class(HWND hwnd);
std::wstring window_title(HWND hwnd);
std::string process_image_name(DWORD pid);
Rect virtual_screen();
// The module brocompositor is linked into (exe or DLL); window classes are
// registered against it.
HINSTANCE module_instance();

}  // namespace brocompositor::win
