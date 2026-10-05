// Shared bits of the scripted test clients (bc_wl_input_client,
// bc_wl_session_client): line output for the test to wait on, and a main
// loop that also reads newline-separated commands from stdin.
#pragma once

#include <wayland-client.h>

#include <poll.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace bctest {

inline void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Dispatches until `running` turns false, the display fails, or stdin closes
// (the test went away). Each complete stdin line goes to `on_line`.
inline int run_client_loop(wl_display* display, const bool& running,
                           const std::function<void(const std::string&)>& on_line) {
    std::string pending;
    while (running) {
        while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
        wl_display_flush(display);
        pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {0, POLLIN, 0}};
        int r = poll(fds, 2, 1000);
        if (r > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) break;
        } else {
            wl_display_cancel_read(display);
        }
        if (r > 0 && (fds[0].revents & (POLLERR | POLLHUP))) break;
        if (r > 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[512];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) break;
            pending.append(buf, size_t(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                on_line(line);
            }
        }
        if (wl_display_dispatch_pending(display) < 0) break;
    }
    return wl_display_get_error(display);
}

}  // namespace bctest
