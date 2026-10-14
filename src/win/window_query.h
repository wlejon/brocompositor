// Which top-level windows are application windows, and what they look like.
#pragma once

#include "brocompositor/events.h"

#include <windows.h>

#include <optional>

namespace brocompositor::win {

class MonitorRegistry;

// Visible, top-level, uncloaked application window (the Alt-Tab population,
// plus captioned owned dialogs). Shell infrastructure is excluded.
bool is_manageable(HWND hwnd);

// Fills everything except id/owner (the tracker's job) and app_id (cached
// by the tracker per window, since it costs a process open).
WindowSnapshot snapshot_window(HWND hwnd, MonitorRegistry& monitors);

// Extracts the window's icon as straight-alpha RGBA8 pixels.
std::optional<WindowIcon> extract_window_icon(HWND hwnd, int32_t desired_size = 0);

// Bitmask of change:: flags between two snapshots of the same window.
uint32_t diff(const WindowSnapshot& before, const WindowSnapshot& after);

}  // namespace brocompositor::win
