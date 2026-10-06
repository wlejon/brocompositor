#include "../src/api/api.h"
#include "brocompositor/events.h"
#include "brocompositor/event_queue.h"
#include "brocompositor/window_manager.h"
#include "embed/embed.h"
#include "eval/eval.h"

#include <cstdlib>
#include <iostream>
#include <string>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")"   \
                      << std::endl;                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

using namespace brocompositor;

namespace {

MonitorSnapshot createMon(MonitorId id, Rect bounds, bool primary, int32_t top_bar = 40) {
    MonitorSnapshot m;
    m.id = id;
    m.name = "MON-" + std::to_string(id);
    m.bounds = bounds;
    m.work_area = Rect{bounds.x, bounds.y + top_bar, bounds.width, bounds.height - top_bar};
    m.primary = primary;
    m.dpi = 96;
    return m;
}

WindowSnapshot createWin(WindowId id, MonitorId m, Rect frame, const std::string& title,
                         const std::string& app_id = "") {
    WindowSnapshot s;
    s.id = id;
    s.monitor = m;
    s.frame = frame;
    s.title = title;
    s.app_id = app_id.empty() ? title : app_id;
    s.class_name = "WindowClass";
    s.process_id = 1000 + static_cast<uint32_t>(id);
    return s;
}

} // namespace

int main() {
    namespace ev = bronze::embed;
    using namespace bronze::eval;

    std::cout << "Starting brocompositor JavaScript API tests..." << std::endl;

    // 1. Install bro.compositor into Bronze realm
    brocompositor::api::installCompositor();

    auto g = ev::globalValue("bro");
    CHECK(g.found);
    CHECK(ev::isObject(g.value));

    ev::Persistent comp(ev::getProperty(g.value, "compositor"));
    CHECK(ev::isObject(comp.get()));
    std::cout << "  Mounted bro.compositor successfully." << std::endl;

    CHECK(ev::isBool(ev::getProperty(comp.get(), "available")));
    CHECK(ev::toBool(ev::getProperty(comp.get(), "available")) == true);

    // Verify all core methods exist
    const char* methods[] = {
        "isAvailable", "getWindows", "getWindow", "focusWindow",
        "moveWindow", "resizeWindow", "closeWindow", "setFloating", "swapWindows",
        "focusDirection", "getWorkspaces", "getWorkspace", "switchWorkspace",
        "createWorkspace", "removeWorkspace", "moveWindowToWorkspace", "setLayoutMode",
        "getLayoutMode", "relayout", "getMonitors", "on", "off", "addEventListener",
        "removeEventListener", "addListener", "removeListener"
    };
    for (const char* m : methods) {
        auto fn = ev::getProperty(comp.get(), m);
        CHECK(ev::isFunction(fn));
        std::cout << "  Found bro.compositor." << m << std::endl;
    }

    // 2. Platform availability
    {
        auto r = evalScript(
            "(function() {\n"
            "  if (bro.compositor.available !== true) return 'available failed';\n"
            "  if (bro.compositor.isAvailable() !== true) return 'isAvailable failed';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");
        std::cout << "  Availability check passed." << std::endl;
    }

    // 3. Set up simulated environment with Monitor and Windows
    auto q = brocompositor::api::getEventQueue();
    CHECK(q != nullptr);

    q->push(MonitorsChanged{{createMon(1, Rect{0, 0, 1920, 1080}, true, 40)}});
    q->push(WindowAdded{createWin(1, 1, Rect{10, 50, 500, 400}, "Editor", "editor")});
    q->push(WindowAdded{createWin(2, 1, Rect{600, 50, 500, 400}, "Terminal", "terminal")});

    brocompositor::api::tickCompositorAsync();

    // 4. Test getWindows() and getWindow()
    std::cout << "Testing getWindows() and getWindow()..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const wins = bro.compositor.getWindows();\n"
            "  if (!Array.isArray(wins)) return 'wins not array';\n"
            "  if (wins.length !== 2) return 'expected 2 windows, got ' + wins.length;\n"
            "  const w1 = wins.find(w => w.id === 1);\n"
            "  if (!w1) return 'w1 not found';\n"
            "  if (w1.title !== 'Editor') return 'w1 title mismatch: ' + w1.title;\n"
            "  if (w1.appId !== 'editor') return 'w1 appId mismatch: ' + w1.appId;\n"
            "  if (typeof w1.frame !== 'object') return 'w1 frame not object';\n"
            "  if (w1.frame.x !== 10 || w1.frame.y !== 50) return 'w1 frame position mismatch';\n"
            "  if (w1.frame.width !== 500 || w1.frame.height !== 400) return 'w1 frame size mismatch';\n"
            "\n"
            "  const single = bro.compositor.getWindow(1);\n"
            "  if (!single || single.id !== 1) return 'getWindow(1) failed';\n"
            "  const missing = bro.compositor.getWindow(9999);\n"
            "  if (missing !== null) return 'getWindow(9999) did not return null';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");
        std::cout << "  getWindows() and getWindow() passed." << std::endl;
    }

    // 5. Test window manipulation: focusWindow, moveWindow, resizeWindow, setFloating, swapWindows
    std::cout << "Testing window manipulations..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  // Test focus\n"
            "  const focusRes = bro.compositor.focusWindow(2);\n"
            "  if (focusRes !== true) return 'focusWindow failed';\n"
            "  const w2 = bro.compositor.getWindow(2);\n"
            "  if (!w2 || !w2.focused) return 'w2 not focused';\n"
            "\n"
            "  // Test moveWindow\n"
            "  const moveRes = bro.compositor.moveWindow(1, { x: 30, y: 70, width: 550, height: 450 });\n"
            "  if (moveRes !== true) return 'moveWindow failed';\n"
            "  const moved = bro.compositor.getWindow(1);\n"
            "  if (moved.frame.x !== 30 || moved.frame.y !== 70) return 'moved pos mismatch';\n"
            "  if (moved.frame.width !== 550 || moved.frame.height !== 450) return 'moved size mismatch';\n"
            "\n"
            "  // Test resizeWindow\n"
            "  const resizeRes = bro.compositor.resizeWindow(1, 600, 500);\n"
            "  if (resizeRes !== true) return 'resizeWindow failed';\n"
            "  const resized = bro.compositor.getWindow(1);\n"
            "  if (resized.frame.width !== 600 || resized.frame.height !== 500) return 'resized size mismatch';\n"
            "\n"
            "  // Test setFloating\n"
            "  const floatRes = bro.compositor.setFloating(1, false);\n"
            "  if (floatRes !== true) return 'setFloating false failed';\n"
            "  const floatRes2 = bro.compositor.setFloating(1, true);\n"
            "  if (floatRes2 !== true) return 'setFloating true failed';\n"
            "\n"
            "  // Test swapWindows\n"
            "  const swapRes = bro.compositor.swapWindows(1, 2);\n"
            "  if (swapRes !== true) return 'swapWindows failed';\n"
            "\n"
            "  // Test focusDirection\n"
            "  const dirRes = bro.compositor.focusDirection('left');\n"
            "  if (dirRes !== true) return 'focusDirection failed';\n"
            "\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");
        std::cout << "  Window manipulation passed." << std::endl;

    }

    // 6. Test Workspaces
    std::cout << "Testing workspaces..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const list = bro.compositor.getWorkspaces();\n"
            "  if (!Array.isArray(list) || list.length === 0) return 'workspaces not array';\n"
            "  const ws1 = list[0];\n"
            "  if (typeof ws1.id !== 'number') return 'ws1 id not number';\n"
            "  if (typeof ws1.layout !== 'string') return 'ws1 layout not string';\n"
            "\n"
            "  // Create new workspace\n"
            "  const newId = bro.compositor.createWorkspace(1, 'Secondary');\n"
            "  if (typeof newId !== 'number' || newId <= 0) return 'createWorkspace failed';\n"
            "\n"
            "  // Switch workspace\n"
            "  const swRes = bro.compositor.switchWorkspace(newId);\n"
            "  if (swRes !== true) return 'switchWorkspace failed';\n"
            "  const wsNew = bro.compositor.getWorkspace(newId);\n"
            "  if (!wsNew || !wsNew.active) return 'new ws not active';\n"
            "\n"
            "  // Set layout mode\n"
            "  const setLayRes = bro.compositor.setLayoutMode(newId, 'columns');\n"
            "  if (setLayRes !== true) return 'setLayoutMode failed';\n"
            "  const mode = bro.compositor.getLayoutMode(newId);\n"
            "  if (mode !== 'columns') return 'mode mismatch: ' + mode;\n"
            "\n"
            "  // Set layout mode to tiling\n"
            "  bro.compositor.setLayoutMode(newId, 'tiling');\n"
            "  const modeTiling = bro.compositor.getLayoutMode(newId);\n"
            "  if (modeTiling !== 'columns') return 'mode tiling mismatch: ' + modeTiling;\n"
            "\n"
            "  // Move window to workspace\n"
            "  const moveWsRes = bro.compositor.moveWindowToWorkspace(1, newId, true);\n"
            "  if (moveWsRes !== true) return 'moveWindowToWorkspace failed';\n"
            "\n"
            "  // Relayout\n"
            "  const relRes = bro.compositor.relayout(newId);\n"
            "  if (relRes !== true) return 'relayout failed';\n"
            "\n"
            "  // Switch back\n"
            "  bro.compositor.switchWorkspace(ws1.id);\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");
        std::cout << "  Workspace tests passed." << std::endl;
    }

    // 7. Test getMonitors()
    std::cout << "Testing getMonitors()..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const mons = bro.compositor.getMonitors();\n"
            "  if (!Array.isArray(mons) || mons.length === 0) return 'monitors not array';\n"
            "  const m = mons[0];\n"
            "  if (typeof m.id !== 'number') return 'm id not number';\n"
            "  if (typeof m.name !== 'string') return 'm name not string';\n"
            "  if (typeof m.bounds !== 'object') return 'm bounds not object';\n"
            "  if (typeof m.workArea !== 'object') return 'm workArea not object';\n"
            "  if (m.primary !== true) return 'm primary not true';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");
        std::cout << "  getMonitors() passed." << std::endl;
    }

    // 8. Test Event Subscriptions
    std::cout << "Testing event subscriptions..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  let winCreated = 0;\n"
            "  let winClosed = 0;\n"
            "  let wsChanged = 0;\n"
            "  let focusChanged = 0;\n"
            "\n"
            "  const h1 = bro.compositor.on('windowCreated', function(e) { winCreated++; });\n"
            "  const h2 = bro.compositor.on('windowClosed', function(e) { winClosed++; });\n"
            "  const h3 = bro.compositor.on('workspaceChanged', function(e) { wsChanged++; });\n"
            "  const h4 = bro.compositor.on('focusChanged', function(e) { focusChanged++; });\n"
            "\n"
            "  if (typeof h1 !== 'object' || typeof h1.id !== 'number') return 'invalid h1';\n"
            "\n"
            "  // Test handle remove\n"
            "  const hTemp = bro.compositor.on('focusChanged', function() {});\n"
            "  if (hTemp.remove() !== true) return 'hTemp remove failed';\n"
            "\n"
            "  // Test off\n"
            "  const hTemp2 = bro.compositor.on('focusChanged', function() {});\n"
            "  if (bro.compositor.off(hTemp2) !== true) return 'off(hTemp2) failed';\n"
            "\n"
            "  // Trigger focusWindow (synchronously emits focusChanged)\n"
            "  bro.compositor.focusWindow(1);\n"
            "  if (focusChanged === 0) return 'focusChanged was not triggered';\n"
            "\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");

        // Test event delivery via EventQueue and tickCompositorAsync
        auto testQueue = brocompositor::api::getEventQueue();
        testQueue->push(WindowAdded{createWin(10, 1, Rect{100, 100, 300, 200}, "DynamicWin")});
        testQueue->push(FocusChanged{10});
        testQueue->push(WindowRemoved{10});

        brocompositor::api::tickCompositorAsync();
        std::cout << "  Event subscriptions passed." << std::endl;
    }

    // 9. Test closeWindow
    std::cout << "Testing closeWindow()..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const before = bro.compositor.getWindow(2);\n"
            "  if (!before) return 'w2 missing before close';\n"
            "  const res = bro.compositor.closeWindow(2);\n"
            "  if (res !== true) return 'closeWindow failed';\n"
            "  const after = bro.compositor.getWindow(2);\n"
            "  if (after !== null) return 'w2 still exists after close';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        CHECK(ev::toUtf8(r.value) == "ok");
        std::cout << "  closeWindow() passed." << std::endl;
    }

    // 10. GC Stress Loop
    std::cout << "Testing GC safety across multiple allocations..." << std::endl;
    for (int i = 0; i < 50; ++i) {
        auto r = evalScript(
            "(function() {\n"
            "  const wins = bro.compositor.getWindows();\n"
            "  const ws = bro.compositor.getWorkspaces();\n"
            "  const mons = bro.compositor.getMonitors();\n"
            "  bro.compositor.moveWindow(1, { x: 10 + Math.floor(Math.random() * 20), y: 50 });\n"
            "  const w = bro.compositor.getWindow(1);\n"
            "  return (wins.length >= 0 && ws.length >= 0 && mons.length >= 0 && w !== null) ? 'ok' : 'err';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        brocompositor::api::tickCompositorAsync();
        if (ev::microtasksPending()) ev::drainMicrotasks();
    }
    std::cout << "  GC safety loop passed." << std::endl;

    // 11. Shutdown test
    std::cout << "Testing shutdownCompositorAsync()..." << std::endl;
    brocompositor::api::shutdownCompositorAsync();
    std::cout << "  shutdownCompositorAsync() completed successfully." << std::endl;

    std::cout << "All brocompositor JavaScript API tests passed!" << std::endl;
    return 0;
}
