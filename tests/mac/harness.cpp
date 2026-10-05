#include "harness.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <thread>

extern char** environ;

namespace bctest {

const brocompositor::mac::Permissions& mac_permissions() {
    static const brocompositor::mac::Permissions p = [] {
        auto q = brocompositor::mac::query_permissions();
        std::printf("permissions: accessibility=%d screen_recording=%d screen_locked=%d responsible=%s (pid %u)\n",
                    q.accessibility, q.screen_recording, q.screen_locked, q.responsible_path.c_str(),
                    q.responsible_pid);
        std::fflush(stdout);
        return q;
    }();
    return p;
}

uint32_t frontmost_pid() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ProcessSerialNumber psn{};
    pid_t pid = 0;
    if (GetFrontProcess(&psn) != noErr || GetProcessPID(&psn, &pid) != noErr) return 0;
#pragma clang diagnostic pop
    return uint32_t(pid);
}

void make_frontmost(uint32_t pid) {
    AXUIElementRef app = AXUIElementCreateApplication(pid_t(pid));
    if (!app) return;
    AXUIElementSetMessagingTimeout(app, 1.0f);
    AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
    CFRelease(app);
}

bool display_awake() {
    uint32_t n = 0;
    return CGGetActiveDisplayList(0, nullptr, &n) == kCGErrorSuccess && n > 0;
}

void skip(const std::string& why) {
    std::printf("SKIP: %s\n", why.c_str());
    std::fflush(stdout);
    std::_Exit(failures() ? 1 : 77);
}

void require_accessibility() {
    if (!mac_permissions().accessibility)
        skip("needs Accessibility: grant it to " + mac_permissions().responsible_path +
             " in System Settings > Privacy & Security > Accessibility");
}

void require_screen_recording() {
    if (!mac_permissions().screen_recording)
        skip("needs Screen Recording: grant it to " + mac_permissions().responsible_path +
             " in System Settings > Privacy & Security > Screen & System Audio Recording");
}

void require_unlocked() {
    if (mac_permissions().screen_locked) skip("the screen is locked (focus and capture need an unlocked session)");
}

void require_display() {
    // The test runner holds a user-activity assertion (caffeinate -u), which
    // wakes a sleeping display within a moment.
    if (!eventually(display_awake, 5000ms)) skip("no display is awake");
}

std::string test_journal_dir() {
    static const std::string dir = [] {
        auto p = std::filesystem::temp_directory_path() / ("bc-journal-" + std::to_string(getpid()));
        std::filesystem::create_directories(p);
        std::atexit([] {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                                            ("bc-journal-" + std::to_string(getpid())),
                                        ec);
        });
        return p.string();
    }();
    return dir;
}

brocompositor::mac::ShellConfig test_shell_config(pid_t pid) {
    brocompositor::mac::ShellConfig c;
    c.process_filter = {uint32_t(pid)};
    c.journal_dir = test_journal_dir();
    return c;
}

bool eventually(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
}

bool near(const Rect& a, const Rect& b, int t) {
    return std::abs(a.x - b.x) <= t && std::abs(a.y - b.y) <= t && std::abs(a.right() - b.right()) <= t &&
           std::abs(a.bottom() - b.bottom()) <= t;
}

TestApp::~TestApp() {
    if (!pid_) return;
    if (in_ >= 0) {
        const char q[] = "quit\n";
        (void)!write(in_, q, sizeof(q) - 1);
        close(in_);
    }
    for (int i = 0; i < 100; ++i) {
        if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
            pid_ = 0;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    if (pid_) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
    }
    if (out_ >= 0) close(out_);
}

bool TestApp::start() {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    char* argv[] = {const_cast<char*>(BC_MAC_TEST_APP), nullptr};
    int rc = posix_spawn(&pid_, BC_MAC_TEST_APP, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in_pipe[0]);
    close(out_pipe[1]);
    in_ = in_pipe[1];
    out_ = out_pipe[0];
    if (rc != 0) {
        pid_ = 0;
        return false;
    }
    return read_line(10000) == "ready";
}

void TestApp::kill_now() {
    if (!pid_) return;
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = 0;
}

std::string TestApp::read_line(int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return line;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return {};
        pollfd p{out_, POLLIN, 0};
        if (poll(&p, 1, int(left.count())) <= 0) continue;
        char buf[512];
        ssize_t n = read(out_, buf, sizeof(buf));
        if (n <= 0) return {};
        buffer_.append(buf, size_t(n));
    }
}

std::string TestApp::cmd(const std::string& line, int timeout_ms) {
    std::string l = line + "\n";
    if (write(in_, l.data(), l.size()) != ssize_t(l.size())) return {};
    return read_line(timeout_ms);
}

uint32_t TestApp::create(const std::string& name, const Rect& f, const std::string& rrggbb) {
    std::ostringstream c;
    c << "create " << name << " " << f.x << " " << f.y << " " << f.width << " " << f.height << " " << rrggbb;
    std::string r = cmd(c.str());
    if (r.rfind("ok ", 0) != 0) return 0;
    return uint32_t(std::strtoul(r.c_str() + 3, nullptr, 10));
}

}  // namespace bctest
