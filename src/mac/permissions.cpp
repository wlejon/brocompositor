// Permission checks. Never prompts: AXIsProcessTrusted (not ...WithOptions
// with the prompt key) and CGPreflightScreenCaptureAccess (not ...Request).
#include "brocompositor/mac/shell_backend.h"
#include "mac/system.h"

#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <libproc.h>
#include <unistd.h>

namespace brocompositor::mac {

namespace {

// libquarantine/libsystem's responsibility SPI: the process TCC attributes
// this one to. Looked up at run time; absent, we report ourselves.
uint32_t responsible_pid() {
    using Fn = pid_t (*)(pid_t);
    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid"));
    pid_t self = getpid();
    pid_t r = fn ? fn(self) : self;
    return uint32_t(r > 0 ? r : self);
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
    p.responsible_path = path_of(p.responsible_pid);
    return p;
}

}  // namespace brocompositor::mac
