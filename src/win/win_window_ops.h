#pragma once

#include "brocompositor/types.h"
#include "brocompositor/window.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace brocompositor {

enum class CornerPreference {
    Default = 0,
    DoNotRound = 1,
    Round = 2,
    RoundSmall = 3
};

class WinWindowOps {
public:
    static bool set_window_rect(WindowId id, const Rect& rect);
    static bool set_window_state(WindowId id, WindowState state);
    static bool focus_window(WindowId id);
    static bool close_window(WindowId id);

    static bool get_window_info(WindowId id, WindowInfo& out_info);
    static bool is_manageable_window(WindowId id);

    // DWM Attributes
    static bool set_dark_mode(WindowId id, bool enable);
    static bool set_corner_preference(WindowId id, CornerPreference pref);
    static bool set_cloaked(WindowId id, bool cloaked);

    // Utility conversions
    static WindowId hwnd_to_id(void* hwnd);
    static void* id_to_hwnd(WindowId id);
};

} // namespace brocompositor
