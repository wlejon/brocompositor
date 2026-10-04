#include "win_hook_manager.h"
#include "win_window_ops.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace brocompositor {

WinHookManager* WinHookManager::s_instance_ = nullptr;

WinHookManager::WinHookManager() {
    s_instance_ = this;
}

WinHookManager::~WinHookManager() {
    stop();
    if (s_instance_ == this) {
        s_instance_ = nullptr;
    }
}

void WinHookManager::set_ignore_process_id(uint32_t pid) {
    ignore_pid_.store(pid);
}

bool WinHookManager::start(WinHookEventCallback callback) {
    if (is_running_.load()) return true;

    callback_ = std::move(callback);
    is_running_.store(true);

    worker_thread_ = std::thread(&WinHookManager::thread_proc, this);

    // Wait until thread ID is initialized
    while (thread_id_.load() == 0 && is_running_.load()) {
        std::this_thread::yield();
    }

    return true;
}

void WinHookManager::stop() {
    if (!is_running_.load()) return;

    is_running_.store(false);
    uint32_t tid = thread_id_.load();
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    thread_id_.store(0);
}

void CALLBACK WinHookManager::win_event_proc(
    void* /*hWinEventHook*/,
    unsigned long event,
    void* hwnd_ptr,
    long idObject,
    long idChild,
    unsigned long /*idEventThread*/,
    unsigned long /*dwmsEventTime*/
) {
    if (!s_instance_ || !s_instance_->callback_) return;

    // Only process window objects (not carats, scrollbars, menus, etc.)
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    HWND hwnd = reinterpret_cast<HWND>(hwnd_ptr);
    if (!hwnd || !IsWindow(hwnd)) return;

    // Check ignore process
    uint32_t ignore_p = s_instance_->ignore_pid_.load();
    if (ignore_p != 0) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ignore_p) return;
    }

    WindowId win_id = WinWindowOps::hwnd_to_id(hwnd);

    WinHookEventType event_type;
    switch (event) {
        case EVENT_OBJECT_CREATE:
            event_type = WinHookEventType::Created;
            break;
        case EVENT_OBJECT_DESTROY:
            event_type = WinHookEventType::Destroyed;
            break;
        case EVENT_SYSTEM_FOREGROUND:
            event_type = WinHookEventType::Foreground;
            break;
        case EVENT_SYSTEM_MOVESIZEEND:
            event_type = WinHookEventType::MoveSizeEnd;
            break;
        case EVENT_SYSTEM_MINIMIZESTART:
            event_type = WinHookEventType::MinimizeStart;
            break;
        case EVENT_SYSTEM_MINIMIZEEND:
            event_type = WinHookEventType::MinimizeEnd;
            break;
        default:
            return;
    }

    // For create and foreground events, ensure window is manageable
    if (event_type == WinHookEventType::Created || event_type == WinHookEventType::Foreground) {
        if (!WinWindowOps::is_manageable_window(win_id)) {
            return;
        }
    }

    WinHookEvent ev{event_type, win_id};
    s_instance_->callback_(ev);
}

void WinHookManager::thread_proc() {
    thread_id_.store(GetCurrentThreadId());

    // Force message queue creation
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // Install hook for object create/destroy
    HWINEVENTHOOK hook_obj = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_DESTROY,
        nullptr,
        reinterpret_cast<WINEVENTPROC>(&WinHookManager::win_event_proc),
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    // Install hook for system events (foreground, movesize, minimize)
    HWINEVENTHOOK hook_sys = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_MINIMIZEEND,
        nullptr,
        reinterpret_cast<WINEVENTPROC>(&WinHookManager::win_event_proc),
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    while (is_running_.load()) {
        BOOL res = GetMessageW(&msg, nullptr, 0, 0);
        if (res == 0 || res == -1) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hook_obj) UnhookWinEvent(hook_obj);
    if (hook_sys) UnhookWinEvent(hook_sys);
}

struct EnumContext {
    std::vector<WindowId>* windows;
    uint32_t ignore_pid;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    if (!hwnd || !IsWindow(hwnd)) return TRUE;

    if (ctx->ignore_pid != 0) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ctx->ignore_pid) return TRUE;
    }

    WindowId id = WinWindowOps::hwnd_to_id(hwnd);
    if (WinWindowOps::is_manageable_window(id)) {
        ctx->windows->push_back(id);
    }
    return TRUE;
}

std::vector<WindowId> WinHookManager::discover_existing_windows() const {
    std::vector<WindowId> wins;
    EnumContext ctx{&wins, ignore_pid_.load()};
    EnumWindows(reinterpret_cast<WNDENUMPROC>(&EnumWindowsCallback), reinterpret_cast<LPARAM>(&ctx));
    return wins;
}

} // namespace brocompositor

#else

namespace brocompositor {

WinHookManager* WinHookManager::s_instance_ = nullptr;
WinHookManager::WinHookManager() {}
WinHookManager::~WinHookManager() {}
void WinHookManager::set_ignore_process_id(uint32_t) {}
bool WinHookManager::start(WinHookEventCallback) { return false; }
void WinHookManager::stop() {}
std::vector<WindowId> WinHookManager::discover_existing_windows() const { return {}; }
void WinHookManager::thread_proc() {}
void CALLBACK WinHookManager::win_event_proc(void*, unsigned long, void*, long, long, unsigned long, unsigned long) {}

} // namespace brocompositor

#endif
