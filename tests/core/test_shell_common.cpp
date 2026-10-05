// The shell backends' portable plumbing: the crash-recovery journal (format,
// atomic rewrite, stale-owner claiming, abandoned claims, malformed files)
// and the per-key serial workers (order per key, isolation between keys,
// bounded shutdown with a stuck job).
#include "check.h"
#include "printers.h"
#include "shell/journal.h"
#include "shell/serial_workers.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <future>
#include <set>
#include <thread>

using namespace brocompositor;
using namespace brocompositor::shell;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path scratch() {
    fs::path p = fs::temp_directory_path() /
                 ("bc-shell-common-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

size_t files_in(const fs::path& d) {
    size_t n = 0;
    for (auto& f : fs::directory_iterator(d)) (void)f, ++n;
    return n;
}

void journal_format() {
    JournalState s;
    s.parked.push_back(ParkedEntry{0x1234, 77, 99999, Rect{10, 20, 300, 200}, Rect{5000, 20, 300, 200}, 1});
    s.parked.push_back(ParkedEntry{0x55, 78, 1, Rect{-100, -50, 640, 480}, Rect{3000, 0, 640, 480}, 2});
    s.reservations = {0xabcdef, 42};
    JournalState back;
    CHECK(Journal::parse(Journal::serialize(s), &back));
    REQUIRE(back.parked.size() == 2);
    CHECK_EQ(back.parked[0].window, uint64_t(0x1234));
    CHECK_EQ(back.parked[0].pid_start, uint64_t(99999));
    CHECK_EQ(back.parked[1].restore, (Rect{-100, -50, 640, 480}));
    CHECK_EQ(back.parked[1].parked, (Rect{3000, 0, 640, 480}));
    CHECK_EQ(back.parked[1].method, uint32_t(2));
    CHECK(back.reservations == s.reservations);
    // Torn or foreign text is rejected.
    std::string text = Journal::serialize(s);
    CHECK(!Journal::parse(text.substr(0, text.size() - 4), &back));
    CHECK(!Journal::parse("something else\n", &back));
    CHECK(!Journal::parse("brocompositor-journal 1\npark 1 2\nend\n", &back));
}

void journal_files() {
    fs::path dir = scratch();
    {
        Journal j(dir, 4242, 777);
        JournalState s;
        s.reservations = {7};
        CHECK(j.write(s));
        CHECK(fs::exists(j.file()));
        CHECK_EQ(files_in(dir), size_t(1));  // no temp file left behind
        s.reservations.clear();
        CHECK(j.write(s));  // empty state: the file goes away
        CHECK(!fs::exists(j.file()));
        CHECK(Journal(fs::path(), 1, 1).write(JournalState{}));  // disabled journal: no-op
    }

    // Owners: 100 alive (start 5), 200 dead, 300's pid reused (start differs).
    auto alive = [](uint32_t pid, uint64_t start) {
        if (pid == 100) return start == 0 || start == 5;
        if (pid == 300) return start == 0 || start == 6;
        return false;
    };
    JournalState s;
    s.parked.push_back(ParkedEntry{1, 2, 3, Rect{0, 0, 10, 10}, Rect{999, 0, 10, 10}, 1});
    Journal(dir, 100, 5).write(s);
    Journal(dir, 200, 5).write(s);
    Journal(dir, 300, 5).write(s);
    {
        std::ofstream junk(dir / "400-1.journal");
        junk << "not a journal";
    }
    std::ofstream(dir / "readme.txt") << "ignored";
    auto stale = Journal::claim_stale(dir, alive, 999);
    std::set<uint32_t> owners;
    for (auto& j : stale) owners.insert(j.pid);
    CHECK((owners == std::set<uint32_t>{200, 300, 400}));
    for (auto& j : stale) {
        if (j.pid == 400) CHECK(j.state.empty());  // malformed: claimed for cleanup
        else CHECK_EQ(j.state.parked.size(), size_t(1));
    }
    // A second claimer finds nothing while the first one (999) lives.
    auto alive_999 = [&](uint32_t pid, uint64_t start) { return pid == 999 || alive(pid, start); };
    CHECK(Journal::claim_stale(dir, alive_999, 998).empty());
    // A claim abandoned by a recoverer that died is picked up again; a live
    // recoverer's claim is not.
    for (auto& j : stale)
        if (j.pid == 200) {
            auto stale2 = Journal::claim_stale(dir, [&](uint32_t pid, uint64_t start) {
                return pid == 999 ? false : alive(pid, start);
            }, 1000);
            CHECK_EQ(stale2.size(), size_t(3));
            for (auto& k : stale2) Journal::discard(k);
        }
    CHECK(fs::exists(dir / "100-5.journal"));  // the live owner's journal is untouched
    CHECK_EQ(files_in(dir), size_t(2));        // it and readme.txt
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void workers() {
    auto pool = SerialWorkers::create({std::chrono::milliseconds(200), nullptr});
    // Order within a key.
    std::mutex m;
    std::vector<int> order;
    std::promise<void> done;
    for (int i = 0; i < 50; ++i)
        pool->post(1, [&, i] {
            std::lock_guard<std::mutex> lock(m);
            order.push_back(i);
            if (i == 49) done.set_value();
        });
    REQUIRE(done.get_future().wait_for(5s) == std::future_status::ready);
    bool sorted = true;
    for (int i = 0; i < 50; ++i) sorted &= order[size_t(i)] == i;
    CHECK(sorted);

    // A blocked key does not delay another key, nor the poster.
    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool open = false;
    std::atomic<bool> other_ran{false};
    auto t0 = std::chrono::steady_clock::now();
    pool->post(2, [&] {
        std::unique_lock<std::mutex> lock(gate_m);
        gate_cv.wait(lock, [&] { return open; });
    });
    pool->post(2, [&] {});  // queued behind the blocked job
    pool->post(3, [&] { other_ran = true; });
    CHECK(std::chrono::steady_clock::now() - t0 < 100ms);
    for (int i = 0; i < 200 && !other_ran; ++i) std::this_thread::sleep_for(5ms);
    CHECK(other_ran.load());
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(pool->stalled(20ms), size_t(1));

    // Shutdown is bounded by its timeout while a job is stuck; the stuck
    // worker finishes later on its own.
    auto s0 = std::chrono::steady_clock::now();
    size_t busy = pool->shutdown(150ms);
    CHECK_EQ(busy, size_t(1));
    CHECK(std::chrono::steady_clock::now() - s0 < 1s);
    CHECK(!pool->post(4, [] {}));
    std::weak_ptr<SerialWorkers> weak = pool;
    pool.reset();
    CHECK(!weak.expired());  // the stuck worker keeps the pool alive
    {
        std::lock_guard<std::mutex> lock(gate_m);
        open = true;
    }
    gate_cv.notify_all();
    for (int i = 0; i < 200 && !weak.expired(); ++i) std::this_thread::sleep_for(5ms);
    CHECK(weak.expired());

    // Idle workers exit and come back on demand.
    auto p2 = SerialWorkers::create({std::chrono::milliseconds(50), nullptr});
    std::atomic<int> n{0};
    p2->post(9, [&] { ++n; });
    std::this_thread::sleep_for(200ms);
    p2->post(9, [&] { ++n; });
    for (int i = 0; i < 200 && n < 2; ++i) std::this_thread::sleep_for(5ms);
    CHECK_EQ(n.load(), 2);
    CHECK_EQ(p2->shutdown(1s), size_t(0));
}

}  // namespace

int main() {
    journal_format();
    journal_files();
    workers();
    return bctest::finish("test_shell_common");
}
