// Vocabulary shared by the shell-role backends (Windows over DWM, macOS over
// WindowServer): backends that manage windows owned by other processes.
//
// Every operation on a foreign window crosses into another process (a
// SetWindowPos that the target's thread must answer, an Accessibility call
// the target app serves), and that process may be hung. Shell backends
// therefore never run such an operation on the caller's thread: each one is
// queued to a worker serving only the target process, and the caller gets a
// Completion it may wait on, poll, or drop. A hung application stalls its
// own worker and nothing else.
#pragma once

#include "brocompositor/geometry.h"

#include <cstddef>
#include <cstdint>
#include <future>

namespace brocompositor {

// Completion of an asynchronous operation. Never waiting on one is fine;
// waiting on one blocks only the waiter.
template <class T>
using Completion = std::shared_future<T>;

enum class FocusResult : uint32_t {
    Focused = 0,         // the window is now the focused (foreground) window
    AlreadyFocused = 1,
    Denied = 2,          // the platform's focus rules refused every strategy
    NoSuchWindow = 3,
    Superseded = 4,      // a later focus request replaced this one before it finished
    Unavailable = 5,     // the backend lacks the capability (macOS: no Accessibility permission)
};

// What the start-up recovery found from instances that died without
// cleaning up (see the backends' journal_dir).
struct RecoveryReport {
    size_t journals = 0;              // stale journals processed
    size_t windows_restored = 0;      // parked/minimized windows put back
    size_t windows_skipped = 0;       // journaled windows gone or moved by their app since
    size_t reservations_removed = 0;  // leaked edge reservations given back
};

// A ready Completion holding `value`.
template <class T>
Completion<T> completed(T value) {
    std::promise<T> p;
    p.set_value(std::move(value));
    return p.get_future().share();
}

}  // namespace brocompositor
