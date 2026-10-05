#include "shell/serial_workers.h"

#include <thread>

namespace brocompositor::shell {

std::shared_ptr<SerialWorkers> SerialWorkers::create(Options options) {
    return std::shared_ptr<SerialWorkers>(new SerialWorkers(std::move(options)));
}

SerialWorkers::~SerialWorkers() = default;

bool SerialWorkers::post(uint64_t key, std::function<void()> job) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    auto it = workers_.find(key);
    if (it == workers_.end()) {
        auto w = std::make_shared<Worker>();
        w->jobs.push_back(std::move(job));
        workers_.emplace(key, w);
        std::thread([self = shared_from_this(), key, w] { self->run(key, w); }).detach();
        return true;
    }
    it->second->jobs.push_back(std::move(job));
    cv_.notify_all();
    return true;
}

void SerialWorkers::run(uint64_t key, std::shared_ptr<Worker> w) {
    if (options_.thread_init) options_.thread_init();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (w->jobs.empty()) {
            if (stopping_) break;
            bool woke = cv_.wait_for(lock, options_.idle_exit, [&] { return !w->jobs.empty() || stopping_; });
            if (!woke) break;  // idle: exit (post() starts a new worker for the key)
            continue;
        }
        std::function<void()> job = std::move(w->jobs.front());
        w->jobs.pop_front();
        w->running_job = true;
        w->job_started = std::chrono::steady_clock::now();
        lock.unlock();
        job();
        job = nullptr;  // release captures outside the lock
        lock.lock();
        w->running_job = false;
    }
    auto it = workers_.find(key);
    if (it != workers_.end() && it->second == w) workers_.erase(it);
    exited_.notify_all();
}

size_t SerialWorkers::shutdown(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    stopping_ = true;
    cv_.notify_all();
    exited_.wait_for(lock, timeout, [&] { return workers_.empty(); });
    return workers_.size();
}

size_t SerialWorkers::stalled(std::chrono::milliseconds age) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    size_t n = 0;
    for (auto& [k, w] : workers_)
        if (w->running_job && now - w->job_started > age) ++n;
    return n;
}

}  // namespace brocompositor::shell
