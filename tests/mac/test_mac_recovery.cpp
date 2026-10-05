// Crash recovery: a backend that dies leaves its parked windows in a journal;
// the next backend with that journal directory puts them back. Without
// Accessibility nothing can be put back, and the journal is handed back for
// a later instance instead of being dropped.
#include "harness.h"

#include <libproc.h>
#include <signal.h>
#include <sys/proc_info.h>
#include <spawn.h>
#include <sys/wait.h>

#include <filesystem>
#include <fstream>
#include <sstream>

extern char** environ;

using namespace bctest;
using namespace brocompositor;
namespace fs = std::filesystem;

namespace {

// A pid that certainly belongs to no live process: a child that exited and
// was reaped.
pid_t dead_pid() {
    pid_t pid = 0;
    char* argv[] = {const_cast<char*>("/usr/bin/true"), nullptr};
    if (posix_spawn(&pid, "/usr/bin/true", nullptr, nullptr, argv, environ) != 0) return 0;
    waitpid(pid, nullptr, 0);
    return pid;
}

// The process start time the backend journals (microseconds since the epoch).
uint64_t start_time(pid_t pid) {
    proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != int(sizeof(info))) return 0;
    return uint64_t(info.pbi_start_tvsec) * 1000000u + uint64_t(info.pbi_start_tvusec);
}

Rect app_frame(TestApp& app, const std::string& name) {
    std::istringstream in(app.cmd("frame " + name));
    std::string ok;
    double x = 0, y = 0, w = 0, h = 0;
    in >> ok >> x >> y >> w >> h;
    return Rect{int32_t(x), int32_t(y), int32_t(w), int32_t(h)};
}

size_t journal_files(const std::string& dir) {
    size_t n = 0;
    for (auto& f : fs::directory_iterator(dir)) n += f.path().extension() == ".journal" ? 1 : 0;
    return n;
}

void stale_journal() {
    std::printf("-- a dead instance's journal\n");
    TestApp app;
    REQUIRE(app.start());
    uint32_t cgid = app.create("kept", Rect{200, 200, 300, 200}, "30C0C0");
    REQUIRE(cgid);
    std::string dir = test_journal_dir() + "/stale";
    fs::create_directories(dir);
    pid_t owner = dead_pid();
    REQUIRE(owner > 0);
    // Two entries: a window of a live application (still on screen: nothing
    // parked it, so nothing to undo) and one of an application that is gone.
    std::string name = dir + "/" + std::to_string(owner) + "-12345.journal";
    {
        std::ofstream f(name);
        f << "brocompositor-journal 1\n"
          << "park " << cgid << " " << app.pid() << " " << start_time(app.pid()) << " 200 200 300 200 "
          << "1495 966 300 200 1\n"
          << "park 99999 " << dead_pid() << " 1 10 10 100 100 2000 2000 100 100 1\n"
          << "end\n";
    }
    auto config = test_shell_config(app.pid());
    config.journal_dir = dir;
    std::string err;
    auto backend = mac::ShellBackend::create(config, &err);
    REQUIRE(backend);
    auto rec = backend->recovery();
    REQUIRE(rec.wait_for(15s) == std::future_status::ready);
    RecoveryReport r = rec.get();
    std::printf("   journals %zu restored %zu skipped %zu\n", r.journals, r.windows_restored, r.windows_skipped);
    CHECK_EQ(r.journals, size_t(1));
    CHECK_EQ(r.windows_restored, size_t(0));
    CHECK_EQ(r.windows_skipped, size_t(2));
    CHECK_EQ(r.reservations_removed, size_t(0));
    if (mac_permissions().accessibility) {
        CHECK_EQ(journal_files(dir), size_t(0));  // processed and removed
    } else {
        // Handed back under its dead owner's name for an instance that has
        // the permission.
        CHECK(fs::exists(name));
    }
    CHECK(near(app_frame(app, "kept"), Rect{200, 200, 300, 200}));
}

void killed_host() {
    if (!mac_permissions().accessibility) {
        std::printf("-- killed host: SKIP (needs Accessibility to park and restore)\n");
        return;
    }
    std::printf("-- killed host\n");
    TestApp app;
    REQUIRE(app.start());
    Rect frame{240, 220, 360, 240};
    uint32_t cgid = app.create("victim", frame, "C0C030");
    REQUIRE(cgid);
    std::string dir = test_journal_dir() + "/killed";
    fs::create_directories(dir);

    int out[2];
    REQUIRE(pipe(out) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::string pid_s = std::to_string(app.pid()), cg_s = std::to_string(cgid);
    char* argv[] = {const_cast<char*>(BC_MAC_SHELL_CHILD), dir.data(), pid_s.data(), cg_s.data(), nullptr};
    pid_t child = 0;
    int rc = posix_spawn(&child, BC_MAC_SHELL_CHILD, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    REQUIRE(rc == 0);
    std::string line;
    char c;
    while (read(out[0], &c, 1) == 1 && c != '\n') line += c;
    close(out[0]);
    CHECK_EQ(line, std::string("parked"));
    CHECK(!near(app_frame(app, "victim"), frame));
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    CHECK_EQ(journal_files(dir), size_t(1));
    CHECK(!near(app_frame(app, "victim"), frame));  // a hard kill undoes nothing by itself

    auto config = test_shell_config(app.pid());
    config.journal_dir = dir;
    std::string err;
    auto backend = mac::ShellBackend::create(config, &err);
    REQUIRE(backend);
    auto rec = backend->recovery();
    REQUIRE(rec.wait_for(15s) == std::future_status::ready);
    RecoveryReport r = rec.get();
    CHECK_EQ(r.journals, size_t(1));
    CHECK_EQ(r.windows_restored, size_t(1));
    CHECK(eventually([&] { return near(app_frame(app, "victim"), frame); }));
    CHECK_EQ(journal_files(dir), size_t(0));
}

}  // namespace

int main() {
    mac_permissions();
    require_display();
    stale_journal();
    killed_host();
    return finish("test_mac_recovery");
}
