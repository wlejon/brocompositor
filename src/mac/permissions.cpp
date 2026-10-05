// Permission checks. Never prompts: AXIsProcessTrusted (not ...WithOptions
// with the prompt key) and CGPreflightScreenCaptureAccess (not ...Request).
#include "brocompositor/mac/shell_backend.h"
#include "mac/system.h"

#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <libproc.h>
#include <mach/mach.h>
#include <unistd.h>

namespace brocompositor::mac {

namespace {

// There is no public API naming the process TCC checks grants against, so
// this uses libquarantine's responsibility SPI (looked up at run time, every
// lookup optional): the responsible pid, and the attributed identity's
// binary path, which is what tccd logs as the request's subject. The two
// differ for launchd jobs that exec another program: a process started over
// ssh is attributed to /usr/libexec/sshd-keygen-wrapper (launchd's ssh
// Program) while the responsible pid runs /usr/libexec/sshd-session.
uint32_t responsible_pid() {
    using Fn = pid_t (*)(pid_t);
    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid"));
    pid_t self = getpid();
    pid_t r = fn ? fn(self) : self;
    return uint32_t(r > 0 ? r : self);
}

std::string attributed_path() {
    using Get = void* (*)(const audit_token_t*, int);
    using Path = const char* (*)(void*);
    using Release = void (*)(void*);
    static Get get = reinterpret_cast<Get>(dlsym(RTLD_DEFAULT, "responsibility_get_attribution_for_audittoken"));
    static Path path = reinterpret_cast<Path>(dlsym(RTLD_DEFAULT, "responsibility_identity_get_binary_path"));
    static Release release = reinterpret_cast<Release>(dlsym(RTLD_DEFAULT, "responsibility_identity_release"));
    if (!get || !path || !release) return {};
    audit_token_t token{};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, reinterpret_cast<task_info_t>(&token), &count) != KERN_SUCCESS)
        return {};
    void* identity = get(&token, 0);
    if (!identity) return {};
    const char* p = path(identity);
    std::string out = p ? p : "";
    release(identity);
    return out;
}

std::string path_of(uint32_t pid) {
    char buf[PROC_PIDPATHINFO_MAXSIZE] = {};
    return proc_pidpath(int(pid), buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
}

}  // namespace

Permissions query_permissions() {
    Permissions p;
    p.accessibility = AXIsProcessTrusted();
    p.screen_recording = CGPreflightScreenCaptureAccess();
    p.screen_locked = sys::screen_locked();
    p.responsible_pid = responsible_pid();
    p.responsible_path = attributed_path();
    if (p.responsible_path.empty()) p.responsible_path = path_of(p.responsible_pid);
    return p;
}

}  // namespace brocompositor::mac
