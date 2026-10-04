#include "win/harness.h"

#include <dwmapi.h>

#include <cstdio>
#include <exception>

#define BC_TEST_APP_W L"" BC_TEST_APP

namespace bctest {

namespace {

BOOL WINAPI on_console_ctrl(DWORD) {
    brocompositor::win::emergency_release_reservations();
    return FALSE;  // continue with default handling (terminate)
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS*) {
    brocompositor::win::emergency_release_reservations();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void init_windows_test() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    SetUnhandledExceptionFilter(on_unhandled);
    std::set_terminate([] {
        brocompositor::win::emergency_release_reservations();
        std::abort();
    });
}

TestApp::~TestApp() {
    if (!pi_.hProcess) return;
    if (in_write_) {
        // Closing stdin makes the app destroy its windows and exit.
        CloseHandle(in_write_);
        in_write_ = nullptr;
    }
    if (WaitForSingleObject(pi_.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi_.hProcess, 1);
    CloseHandle(pi_.hProcess);
    CloseHandle(pi_.hThread);
    if (out_read_) CloseHandle(out_read_);
}

bool TestApp::start() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write_, &sa, 0) || !CreatePipe(&out_read_, &out_write, &sa, 0)) return false;
    SetHandleInformation(in_write_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read_, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring cmdline = L"\"" + std::wstring(BC_TEST_APP_W) + L"\"";
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi_);
    CloseHandle(in_read);
    CloseHandle(out_write);
    return ok != FALSE;
}

std::string TestApp::cmd(const std::string& line) {
    std::string l = line + "\n";
    DWORD n = 0;
    if (!WriteFile(in_write_, l.data(), DWORD(l.size()), &n, nullptr)) return "err write";
    std::string reply;
    char c;
    while (ReadFile(out_read_, &c, 1, &n, nullptr) && n == 1) {
        if (c == '\n') break;
        reply.push_back(c);
    }
    return reply;
}

HWND TestApp::create(const std::string& name, const Rect& r, const std::string& rgb, const std::string& flags) {
    std::string reply = cmd("create " + name + " " + std::to_string(r.x) + " " + std::to_string(r.y) + " " +
                            std::to_string(r.width) + " " + std::to_string(r.height) + " " + rgb + " " + flags);
    if (reply.rfind("ok ", 0) != 0) {
        std::fprintf(stderr, "create %s: %s\n", name.c_str(), reply.c_str());
        return nullptr;
    }
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(reply.substr(3))));
}

void EventLog::pump() {
    for (auto& e : queue_.drain()) {
        all_.push_back(std::move(e));
        used_.push_back(false);
    }
}

void EventLog::settle(std::chrono::milliseconds quiet) {
    pump();
    while (queue_.wait_for(quiet)) pump();
}

ForegroundGuard::ForegroundGuard() : previous_(GetForegroundWindow()) {}

ForegroundGuard::~ForegroundGuard() {
    if (previous_ && IsWindow(previous_)) force_foreground(previous_);
}

bool force_foreground(HWND hwnd) {
    if (GetForegroundWindow() == hwnd) return true;
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return true;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    SetForegroundWindow(hwnd);
    return wait_until([&] { return GetForegroundWindow() == hwnd; }, 500ms);
}

Rect frame_of(HWND hwnd) {
    RECT r{};
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r));
    return Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
}

Rect outer_of(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
}

Rect primary_work_area() {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return Rect{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
}

Rect virtual_screen_rect() {
    return Rect{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

std::vector<Rect> monitor_work_areas() {
    std::vector<Rect> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(m, &mi);
            reinterpret_cast<std::vector<Rect>*>(p)->push_back(
                Rect{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left,
                     mi.rcWork.bottom - mi.rcWork.top});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

bool interactive_desktop() {
    HDESK d = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!d) return false;
    wchar_t name[64] = L"";
    DWORD n = 0;
    GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &n);
    CloseDesktop(d);
    return std::wstring(name) == L"Default";
}

bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        // Keep this thread's message queue serviced (cross-thread sends).
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
    return true;
}

}  // namespace bctest
