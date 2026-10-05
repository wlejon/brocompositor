// Crash-recovery journal for the shell backends.
//
// A shell backend changes state that outlives it when it is killed: windows
// it parked off-screen or minimized for a hidden workspace, and (Windows)
// appbar edge reservations held by the shell. Each backend instance keeps
// one journal file describing exactly that state, rewritten atomically on
// every change and removed on a clean shutdown:
//
//   <dir>/<owner pid>-<owner start time>.journal
//
// On start a backend looks for journals whose owner process is gone (the
// pid is dead, or reused by a process with a different start time), claims
// each one by renaming it (so two starting instances never recover the same
// file), undoes what it describes, and deletes it. Live owners' journals are
// never touched, so instances sharing a directory do not interfere.
//
// Portable: std::filesystem only; process liveness is the backend's callback.
#pragma once

#include "brocompositor/geometry.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace brocompositor::shell {

struct ParkedEntry {
    uint64_t window = 0;     // native handle (HWND / CGWindowID)
    uint32_t pid = 0;        // owning process
    uint64_t pid_start = 0;  // its start time (detects pid reuse)
    Rect restore;            // frame to put back
    Rect parked;             // where the backend put it (recovery only acts if it is still there)
    uint32_t method = 0;     // backend-specific hide method
};

struct JournalState {
    std::vector<ParkedEntry> parked;
    std::vector<uint64_t> reservations;  // Windows: appbar HWNDs registered with the shell
    bool empty() const { return parked.empty() && reservations.empty(); }
};

struct StaleJournal {
    std::filesystem::path file;  // the claimed file (delete it with discard())
    std::string name;            // its name before it was claimed (to hand it back under)
    uint32_t pid = 0;
    uint64_t pid_start = 0;
    JournalState state;
};

class Journal {
public:
    // An empty `dir` disables journaling (writes are no-ops). The file is
    // <pid>-<start>.journal, or <pid>-<start>-<n>.journal for the n-th
    // instance alive at once in one process (several backends).
    Journal(std::filesystem::path dir, uint32_t pid, uint64_t pid_start);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    // Atomically replaces this instance's journal; an empty state removes it.
    bool write(const JournalState& state);
    void remove();
    const std::filesystem::path& file() const { return file_; }
    bool enabled() const { return !file_.empty(); }

    // Journals in `dir` whose owner `alive(pid, start)` says is gone, each
    // already claimed by this caller (also journals claimed by a recoverer
    // that died; alive(pid, 0) asks whether any process has that pid).
    // Malformed files are claimed too (with an empty state) so they get
    // cleaned up.
    static std::vector<StaleJournal> claim_stale(const std::filesystem::path& dir,
                                                 const std::function<bool(uint32_t, uint64_t)>& alive,
                                                 uint32_t self_pid);
    static void discard(const StaleJournal& j);

    static std::string serialize(const JournalState& state);
    static bool parse(const std::string& text, JournalState* out);

private:
    std::filesystem::path dir_;
    std::filesystem::path file_;
    std::mutex mutex_;
};

}  // namespace brocompositor::shell
