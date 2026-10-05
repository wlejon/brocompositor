#include "linux/dispatcher.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>

namespace brocompositor::wl {

Dispatcher::Dispatcher() { fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); }

Dispatcher::~Dispatcher() {
    close();
    if (fd_ >= 0) ::close(fd_);
}

bool Dispatcher::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (closed_) return false;
        jobs_.push_back(std::move(job));
    }
    uint64_t one = 1;
    ssize_t r = write(fd_, &one, sizeof one);
    (void)r;
    return true;
}

void Dispatcher::run_pending() {
    uint64_t n = 0;
    ssize_t r = read(fd_, &n, sizeof n);
    (void)r;
    for (;;) {
        std::vector<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> lock(m_);
            jobs.swap(jobs_);
        }
        if (jobs.empty()) return;
        for (auto& j : jobs) j();
    }
}

void Dispatcher::close() {
    std::vector<std::function<void()>> dropped;
    {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        dropped.swap(jobs_);
    }
    // Destroying the jobs breaks the promises of waiting call()ers.
}

}  // namespace brocompositor::wl
