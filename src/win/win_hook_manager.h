#pragma once

#include "brocompositor/types.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#ifndef CALLBACK
#define CALLBACK
#endif
#endif

#include <functional>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

namespace brocompositor {

enum class WinHookEventType {
    Created,
    Destroyed,
    Foreground,
    MoveSizeEnd,
    MinimizeStart,
    MinimizeEnd
};

struct WinHookEvent {
    WinHookEventType type;
    WindowId window_id;
};

using WinHookEventCallback = std::function<void(const WinHookEvent&)>;

class WinHookManager {
public:
    WinHookManager();
    ~WinHookManager();

    bool start(WinHookEventCallback callback);
    void stop();
    bool is_running() const { return is_running_.load(); }

    // Enumerate existing windows on the desktop
    std::vector<WindowId> discover_existing_windows() const;

    // Filter configuration
    void set_ignore_process_id(uint32_t pid);

private:
    void thread_proc();
    static void CALLBACK win_event_proc(
        void* hWinEventHook,
        unsigned long event,
        void* hwnd,
        long idObject,
        long idChild,
        unsigned long idEventThread,
        unsigned long dwmsEventTime
    );

    std::atomic<bool> is_running_{false};
    std::atomic<uint32_t> thread_id_{0};
    std::thread worker_thread_;

    WinHookEventCallback callback_;
    std::atomic<uint32_t> ignore_pid_{0};

    static WinHookManager* s_instance_;
};

} // namespace brocompositor
