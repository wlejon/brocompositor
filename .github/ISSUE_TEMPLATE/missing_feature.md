---
name: Missing protocol or feature
about: A Wayland protocol the server does not offer, or a window operation a shell backend cannot do
labels: enhancement
---

**What is missing:** the protocol (and version), or the window operation and
the backend it is missing from.

**The client or application that needs it:** what it does without it (falls
back, refuses to start, misbehaves). A real application name and version is
the most useful answer; it is what decides which gaps get closed first.

**How others do it:** a compositor or window manager that already supports it
(sway, KWin, mutter, DWM, the macOS window server), if you know.
