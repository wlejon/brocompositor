// Accumulates events from an EventQueue and waits for matching ones.
// Portable; shared by the Windows and macOS shell-backend tests.
#pragma once

#include "brocompositor/event_queue.h"

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

namespace bctest {

using namespace std::chrono_literals;

class EventLog {
public:
    explicit EventLog(brocompositor::EventQueue& q) : queue_(q) {}

    // Pumps until pred matches an event (after the last mark()) that no
    // previous wait consumed; consumes and returns it.
    template <class T>
    std::optional<T> wait(const std::function<bool(const T&)>& pred,
                          std::chrono::milliseconds timeout = 3000ms) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump();
            for (size_t i = from_; i < all_.size(); ++i) {
                if (used_[i]) continue;
                if (auto* e = std::get_if<T>(&all_[i]); e && pred(*e)) {
                    used_[i] = true;
                    return *e;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            queue_.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        }
    }

    // Events of type T (after the last mark()) matching pred, without consuming.
    template <class T>
    std::vector<T> collect(const std::function<bool(const T&)>& pred) {
        pump();
        std::vector<T> out;
        for (size_t i = from_; i < all_.size(); ++i)
            if (auto* e = std::get_if<T>(&all_[i]); e && pred(*e)) out.push_back(*e);
        return out;
    }

    // Lets events keep arriving until `quiet` passes with none.
    void settle(std::chrono::milliseconds quiet = 300ms) {
        pump();
        while (queue_.wait_for(quiet)) pump();
    }
    // Later waits/collects only look at events after this point.
    void mark() {
        pump();
        from_ = all_.size();
    }
    void pump() {
        for (auto& e : queue_.drain()) {
            all_.push_back(std::move(e));
            used_.push_back(false);
        }
    }
    const std::vector<brocompositor::Event>& all() const { return all_; }

private:
    brocompositor::EventQueue& queue_;
    std::vector<brocompositor::Event> all_;
    std::vector<bool> used_;
    size_t from_ = 0;
};

}  // namespace bctest
