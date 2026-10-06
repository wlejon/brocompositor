// Crash recovery: a shell host (bc_win_shell_child) parks a window of
// bc_test_app and reserves a work-area strip, then is killed with
// TerminateProcess, so no destructor, handler or atexit runs. Its journal
// survives; a new ShellBackend on the same journal directory must leave a
// live owner's journal alone, and once the owner is dead put the window back
// and give the strip back. The work area of every monitor is compared with
// its original at the end; if recovery failed, the test removes the leaked
// reservation itself (from the journal) so the desktop is left as found.
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef BC_WIN_SHELL_CHILD
#error BC_WIN_SHELL_CHILD must name the shell child
#endif
#define BC_WIN_SHELL_CHILD_W L"" BC_WIN_SHELL_CHILD

using namespace brocompositor;
using namespace bctest;

namespace {

bool on_any_monitor(HWND h) {
    Rect f = frame_of(h);
    RECT r{f.x, f.y, f.right(), f.bottom()};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

struct Child {
    PROCESS_INFORMATION pi{};
    HANDLE out = nullptr;
    bool start(const std::wstring& args) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE w = nullptr;
        if (!CreatePipe(&out, &w, &sa, 0)) return false;
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        std::wstring cmd = L"\"" + std::wstring(BC_WIN_SHELL_CHILD_W) + L"\" " + args;
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &pi);
        CloseHandle(w);
        return ok != FALSE;
    }
    std::string line() {
        std::string s;
        char c;
        DWORD n = 0;
        while (ReadFile(out, &c, 1, &n, nullptr) && n == 1 && c != '\n') s.push_back(c);
        return s;
    }
    void kill() {
        if (!pi.hProcess) return;
        TerminateProcess(pi.hProcess, 9);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    ~Child() {
        kill();
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        if (out) CloseHandle(out);
    }
};

std::vector<uint64_t> journaled_reservations(const std::string& dir) {
    std::vector<uint64_t> out;
    std::error_code ec;
    for (auto& f : std::filesystem::directory_iterator(dir, ec)) {
        std::ifstream in(f.path());
        std::string l;
        while (std::getline(in, l)) {
            std::istringstream s(l);
            std::string kind;
            uint64_t v = 0;
            if (s >> kind >> v && kind == "reservation") out.push_back(v);
        }
    }
    return out;
}

void run() {
    std::vector<Rect> original = monitor_work_areas();
    TestApp app;
    REQUIRE(app.start());
    Rect wa = primary_work_area();
    Rect home{wa.x + 140, wa.y + 120, 600, 400};
    HWND h = app.create("victim", home, "5522aa");
    REQUIRE(h);
    Rect home_frame = frame_of(h);
    std::string dir = test_journal_dir();

    Child child;
    REQUIRE(child.start(std::wstring(dir.begin(), dir.end()) + L" " + std::to_wstring(app.pid()) + L" " +
                        std::to_wstring(uint64_t(reinterpret_cast<uintptr_t>(h)))));
    std::string ready = child.line();
    std::printf("  child: %s\n", ready.c_str());
    REQUIRE(ready.rfind("ready", 0) == 0);
    CHECK(!on_any_monitor(h));
    CHECK(primary_work_area() != wa);
    auto leaked = journaled_reservations(dir);
    CHECK_EQ(leaked.size(), size_t(1));

    // A backend started while the owner lives does not touch its journal.
    {
        auto bystander = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
        REQUIRE(bystander);
        auto r = bystander->recovery().get();
        CHECK_EQ(r.journals, size_t(0));
        CHECK(!on_any_monitor(h));
    }

    // Hard kill: nothing of the child's cleanup runs.
    child.kill();
    Sleep(500);
    bool parked_after_kill = !on_any_monitor(h);
    bool strip_after_kill = primary_work_area() != wa;
    std::printf("  after TerminateProcess: window %s, work-area strip %s\n",
                parked_after_kill ? "still parked" : "back on screen",
                strip_after_kill ? "still reserved" : "given back by the shell");
    CHECK(parked_after_kill);

    // The next instance recovers.
    {
        auto next = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
        REQUIRE(next);
        auto r = next->recovery();
        REQUIRE(r.wait_for(15s) == std::future_status::ready);
        auto rep = r.get();
        std::printf("  recovery: %zu journal(s), %zu window(s) restored, %zu skipped, %zu reservation(s) removed\n",
                    rep.journals, rep.windows_restored, rep.windows_skipped, rep.reservations_removed);
        CHECK_EQ(rep.journals, size_t(1));
        CHECK_EQ(rep.windows_restored, size_t(1));
        CHECK_EQ(rep.reservations_removed, size_t(1));
        CHECK(wait_until([&] { return frame_of(h) == home_frame; }));
        CHECK_EQ(frame_of(h), home_frame);
        CHECK(wait_until([&] { return monitor_work_areas() == original; }, 5000ms));
        // The stale journal is gone; this instance holds nothing.
        std::error_code ec;
        size_t files = 0;
        for (auto& f : std::filesystem::directory_iterator(dir, ec)) (void)f, ++files;
        CHECK_EQ(files, size_t(0));
        // A second recovery finds nothing left to do.
        auto again = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
        REQUIRE(again);
        CHECK_EQ(again->recovery().get().journals, size_t(0));
    }

    // Leave the desktop as found even if recovery failed.
    if (monitor_work_areas() != original) {
        for (uint64_t r : leaked) {
            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            abd.hWnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(r));
            SHAppBarMessage(ABM_REMOVE, &abd);
        }
        wait_until([&] { return monitor_work_areas() == original; }, 5000ms);
    }
    CHECK_EQ(monitor_work_areas(), original);
}

}  // namespace

int main() {
    bctest::require_mutate("test_win_recovery", "reserves a work-area strip from a process it then kills");
    init_windows_test();
    run();
    return finish("test_win_recovery");
}
