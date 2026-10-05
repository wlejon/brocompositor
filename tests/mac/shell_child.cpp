// bc_mac_shell_child <journal dir> <app pid> <CGWindowID>: a shell host that
// parks one window and then waits to be killed (test_mac_recovery).
#include "brocompositor/mac/shell_backend.h"
#include "event_log.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace brocompositor;

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    mac::ShellConfig config;
    config.journal_dir = argv[1];
    config.process_filter = {uint32_t(std::strtoul(argv[2], nullptr, 10))};
    config.recover = false;
    uint64_t cgid = std::strtoull(argv[3], nullptr, 10);
    std::string err;
    auto backend = mac::ShellBackend::create(config, &err);
    if (!backend) {
        std::printf("error %s\n", err.c_str());
        return 1;
    }
    bctest::EventLog log(backend->events());
    auto added = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == cgid; },
                                       std::chrono::milliseconds(5000));
    if (!added) {
        std::printf("error not-found\n");
        return 1;
    }
    auto hidden = backend->set_visible(added->window.id, false);
    if (hidden.wait_for(std::chrono::seconds(5)) != std::future_status::ready || !hidden.get()) {
        std::printf("error park\n");
        return 1;
    }
    std::printf("parked\n");
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
