// EventQueue: many producers, one draining consumer, per-producer order kept.
#include "brocompositor/event_queue.h"

#include "check.h"

#include <atomic>
#include <thread>

using namespace brocompositor;

int main() {
    EventQueue q;
    std::atomic<int> wakes{0};
    q.set_wake([&] { wakes.fetch_add(1); });

    constexpr int kProducers = 4, kPerProducer = 5000;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i)
                q.push(WindowRemoved{WindowId(uint64_t(p) << 32 | uint64_t(i))});
        });
    }

    std::vector<uint64_t> next(kProducers, 0);
    size_t received = 0;
    bool in_order = true;
    auto consume = [&] {
        for (auto& e : q.drain()) {
            auto* r = std::get_if<WindowRemoved>(&e);
            if (!r) {
                in_order = false;
                continue;
            }
            uint64_t p = r->id >> 32, i = r->id & 0xffffffffu;
            if (p >= kProducers || next[p] != i) in_order = false;
            if (p < kProducers) next[p] = i + 1;
            ++received;
        }
    };
    // Consume concurrently with the producers; give up after ~10 s of silence.
    for (int idle = 0; received < size_t(kProducers * kPerProducer) && idle < 100;) {
        idle = q.wait_for(std::chrono::milliseconds(100)) ? 0 : idle + 1;
        consume();
    }
    for (auto& t : producers) t.join();
    consume();

    CHECK_EQ(received, size_t(kProducers * kPerProducer));
    CHECK(in_order);
    CHECK_EQ(wakes.load(), kProducers * kPerProducer);
    CHECK_EQ(q.size(), size_t(0));
    CHECK(!q.wait_for(std::chrono::milliseconds(1)));
    return bctest::finish("test_event_queue");
}
