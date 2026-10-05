// Serial job queues keyed by an integer (a target process id), each served by
// its own thread, so a job that blocks on one key (a hung application)
// never delays jobs for another key or the thread that posted it.
//
// Workers start on demand and exit after an idle period. Threads are
// detached and keep the pool alive through a shared_ptr, so a worker stuck
// in a call into a hung process can outlive shutdown() safely: jobs must
// capture (by shared_ptr) everything they touch.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace brocompositor::shell {

class SerialWorkers : public std::enable_shared_from_this<SerialWorkers> {
public:
    struct Options {
        std::chrono::milliseconds idle_exit{10000};
        std::function<void()> thread_init;  // runs first on every worker thread
    };

    static std::shared_ptr<SerialWorkers> create(Options options);
    ~SerialWorkers();

    // Queues `job` for `key`. False after shutdown() (the job is dropped).
    bool post(uint64_t key, std::function<void()> job);

    // Stops accepting jobs, lets the queued ones run, and waits up to
    // `timeout` for every worker to exit. Returns how many are still busy
    // (blocked in a job); they finish on their own and then exit.
    size_t shutdown(std::chrono::milliseconds timeout);

    // Keys whose worker has been running one job for longer than `age`.
    size_t stalled(std::chrono::milliseconds age) const;

private:
    struct Worker {
        std::deque<std::function<void()>> jobs;
        bool running_job = false;
        std::chrono::steady_clock::time_point job_started;
    };
    explicit SerialWorkers(Options options) : options_(std::move(options)) {}
    void run(uint64_t key, std::shared_ptr<Worker> w);

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;       // jobs posted / shutdown
    std::condition_variable exited_;   // a worker exited
    std::map<uint64_t, std::shared_ptr<Worker>> workers_;
    bool stopping_ = false;
};

}  // namespace brocompositor::shell
