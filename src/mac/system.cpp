// CoreGraphics / libproc half of mac/system.h.
#include "mac/system.h"

#include "mac/cf.h"

#include <libproc.h>
#include <signal.h>
#include <sys/proc_info.h>

#include <cerrno>
#include <cmath>

namespace brocompositor::mac::sys {

namespace {

Rect bounds_of(CFDictionaryRef d) {
    CGRect r{};
    auto b = static_cast<CFDictionaryRef>(CFDictionaryGetValue(d, kCGWindowBounds));
    if (!b || !CGRectMakeWithDictionaryRepresentation(b, &r)) return Rect{};
    int32_t x = int32_t(std::lround(r.origin.x)), y = int32_t(std::lround(r.origin.y));
    return Rect{x, y, int32_t(std::lround(r.origin.x + r.size.width)) - x,
                int32_t(std::lround(r.origin.y + r.size.height)) - y};
}

CgWindow parse(CFDictionaryRef d) {
    CgWindow w;
    w.id = uint32_t(dict_int(d, kCGWindowNumber).value_or(0));
    w.pid = uint32_t(dict_int(d, kCGWindowOwnerPID).value_or(0));
    w.layer = int32_t(dict_int(d, kCGWindowLayer).value_or(0));
    w.frame = bounds_of(d);
    w.alpha = dict_double(d, kCGWindowAlpha).value_or(1.0);
    w.onscreen = dict_bool(d, kCGWindowIsOnscreen);
    w.title = dict_string(d, kCGWindowName);
    w.owner = dict_string(d, kCGWindowOwnerName);
    return w;
}

}  // namespace

std::vector<CgWindow> window_list(bool onscreen_only) {
    std::vector<CgWindow> out;
    CGWindowListOption opt = (onscreen_only ? kCGWindowListOptionOnScreenOnly : kCGWindowListOptionAll) |
                             kCGWindowListExcludeDesktopElements;
    CFRef<CFArrayRef> list(CGWindowListCopyWindowInfo(opt, kCGNullWindowID));
    if (!list) return out;
    CFIndex n = CFArrayGetCount(list.get());
    out.reserve(size_t(n));
    for (CFIndex i = 0; i < n; ++i)
        out.push_back(parse(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list.get(), i))));
    return out;
}

std::optional<CgWindow> describe_window(uint32_t id) {
    const void* value = reinterpret_cast<const void*>(uintptr_t(id));
    CFRef<CFArrayRef> ids(CFArrayCreate(nullptr, &value, 1, nullptr));
    CFRef<CFArrayRef> list(CGWindowListCreateDescriptionFromArray(ids.get()));
    if (!list || CFArrayGetCount(list.get()) == 0) return std::nullopt;
    CgWindow w = parse(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list.get(), 0)));
    if (w.id != id) return std::nullopt;
    return w;
}

uint64_t process_start_time(uint32_t pid) {
    proc_bsdinfo info{};
    if (pid == 0 || proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != int(sizeof(info))) return 0;
    return uint64_t(info.pbi_start_tvsec) * 1000000u + uint64_t(info.pbi_start_tvusec);
}

bool process_alive(uint32_t pid, uint64_t start) {
    if (pid == 0) return false;
    // EPERM: exists but not ours to signal. Never recover the journal of an
    // owner we cannot rule out.
    if (kill(pid_t(pid), 0) != 0 && errno != EPERM) return false;
    if (start == 0) return true;
    uint64_t now = process_start_time(pid);
    return now == 0 || now == start;
}

bool screen_locked() {
    CFRef<CFDictionaryRef> session(CGSessionCopyCurrentDictionary());
    if (!session) return true;  // no GUI session at all
    return dict_bool(session.get(), CFSTR("CGSSessionScreenIsLocked"));
}

}  // namespace brocompositor::mac::sys
