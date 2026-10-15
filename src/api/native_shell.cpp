// bro.compositor's desktop-shell role: what a host that is the session's
// shell over a system compositor (helm as the Windows Winlogon shell) adds
// to the window-management API. The host supplies it (setShellRole); an
// ordinary app answers false.
#include "host_compositor_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

namespace brocompositor::api {

void installShellOnto(Value compObj) {
    ObjectBuilder comp(compObj);

    // bro.compositor.isDesktopShell() -> boolean
    comp.def("isDesktopShell", 0, [](Value, std::span<const Value>) -> Value {
        auto role = shellRole();
        return ev::fromBool(role.is_desktop_shell && role.is_desktop_shell());
    });

    // bro.compositor.setShellOverlay(active) -> boolean (whether the host acted)
    comp.def("setShellOverlay", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto role = shellRole();
        return ev::fromBool(role.set_overlay && role.set_overlay(ev::toBool(reader.get(0))));
    });
}

}  // namespace brocompositor::api
