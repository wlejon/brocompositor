// bc_test_app: owns the windows the Windows-backend tests manage, in a
// process separate from the test (so every backend path is the real
// cross-process one). Line protocol on stdin, one reply line on stdout:
//
//   create <name> <x> <y> <w> <h> <rrggbb> [hidden] [tool] [owner=<name>]  -> ok <hwnd>
//   show|hide|minimize|maximize|restore|destroy|repaint <name>             -> ok
//   title <name> <text...>                                                 -> ok
//   color <name> <rrggbb>                                                  -> ok
//   move <name> <x> <y> <w> <h>        (outer window rect)                 -> ok
//   hang <ms>      replies, then the UI thread stops pumping for <ms>      -> ok
//   exit
//
// The app exits when stdin closes, so a crashed test never leaves windows.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

namespace {

struct Win {
    HWND hwnd = nullptr;
    COLORREF color = 0;
};

std::map<std::string, Win> g_windows;
DWORD g_main_thread = 0;

COLORREF parse_color(const std::string& hex) {
    unsigned v = unsigned(std::stoul(hex, nullptr, 16));
    return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            COLORREF c = COLORREF(GetWindowLongPtrW(h, GWLP_USERDATA));
            HBRUSH b = CreateSolidBrush(c);
            RECT r;
            GetClientRect(h, &r);
            FillRect(dc, &r, b);
            DeleteObject(b);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring widen(const std::string& s) {
    std::wstring w(s.size() * 2 + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
    w.resize(size_t(n));
    return w;
}

std::string run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, name;
    in >> cmd >> name;
    if (cmd == "create") {
        int x, y, w, h;
        std::string color, flag;
        in >> x >> y >> w >> h >> color;
        bool hidden = false;
        DWORD ex = 0;
        HWND owner = nullptr;
        while (in >> flag) {
            if (flag == "hidden") hidden = true;
            else if (flag == "tool") ex |= WS_EX_TOOLWINDOW;
            else if (flag.rfind("owner=", 0) == 0) owner = g_windows[flag.substr(6)].hwnd;
        }
        HWND hwnd = CreateWindowExW(ex, L"bc_test_app", widen(name).c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h,
                                    owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return "err create " + std::to_string(GetLastError());
        COLORREF c = parse_color(color);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(c));
        g_windows[name] = Win{hwnd, c};
        if (!hidden) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        return "ok " + std::to_string(uint64_t(reinterpret_cast<uintptr_t>(hwnd)));
    }
    if (cmd == "exit") {
        PostQuitMessage(0);
        return "ok";
    }
    auto it = g_windows.find(name);
    if (it == g_windows.end()) return "err no window " + name;
    HWND hwnd = it->second.hwnd;
    if (cmd == "show") ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    else if (cmd == "hide") ShowWindow(hwnd, SW_HIDE);
    else if (cmd == "minimize") ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    else if (cmd == "maximize") ShowWindow(hwnd, SW_MAXIMIZE);
    else if (cmd == "restore") ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    else if (cmd == "destroy") {
        DestroyWindow(hwnd);
        g_windows.erase(it);
    } else if (cmd == "repaint") {
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    } else if (cmd == "title") {
        std::string rest;
        std::getline(in, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        SetWindowTextW(hwnd, widen(rest).c_str());
    } else if (cmd == "color") {
        std::string color;
        in >> color;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(parse_color(color)));
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    } else if (cmd == "move") {
        int x, y, w, h;
        in >> x >> y >> w >> h;
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        return "err unknown command " + cmd;
    }
    return "ok";
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_main_thread = GetCurrentThreadId();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"bc_test_app";
    RegisterClassExW(&wc);

    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue
    std::thread reader([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            PostThreadMessageW(g_main_thread, WM_APP, 0, reinterpret_cast<LPARAM>(new std::string(line)));
        }
        PostThreadMessageW(g_main_thread, WM_APP + 1, 0, 0);  // stdin closed
    });
    reader.detach();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_APP) {
            auto* line = reinterpret_cast<std::string*>(msg.lParam);
            // hang <ms>: reply, then stop pumping messages (a hung application).
            int hang_ms = 0;
            if (line->rfind("hang ", 0) == 0) hang_ms = std::atoi(line->c_str() + 5);
            std::string reply = hang_ms > 0 ? "ok" : run(*line);
            delete line;
            std::fwrite(reply.data(), 1, reply.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
            if (hang_ms > 0) Sleep(DWORD(hang_ms));
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_APP + 1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (auto& [name, w] : g_windows) DestroyWindow(w.hwnd);
    return 0;
}
