#pragma once

#include "brocompositor/types.h"
#include "brocompositor/appbar.h"
#include <functional>
#include <unordered_map>

namespace brocompositor {

using WinAppBarCallback = std::function<void(AppBarId, const Rect& new_work_area)>;

class WinAppBar {
public:
    WinAppBar();
    ~WinAppBar();

    bool register_bar(AppBarId id, void* hwnd, Edge edge, int32_t thickness);
    bool unregister_bar(AppBarId id);
    bool update_pos(AppBarId id);

    static Rect get_system_work_area();
    static bool set_system_work_area(const Rect& area);

    void set_callback(WinAppBarCallback cb);

    // Notification handler for shell callbacks (ABN_POSCHANGED, etc.)
    void on_appbar_notification(AppBarId id, uint32_t notification);

private:
    struct NativeBarRecord {
        AppBarId id;
        void* hwnd = nullptr;
        Edge edge = Edge::Top;
        int32_t thickness = 32;
        Rect allocated_rect;
    };

    std::unordered_map<AppBarId, NativeBarRecord> registered_bars_;
    WinAppBarCallback callback_;
};

} // namespace brocompositor
