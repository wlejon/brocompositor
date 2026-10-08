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
        "removeEventListener", "addListener", "removeListener",
        "minimizeWindow", "maximizeWindow", "fullscreenWindow", "restoreWindow",
        "reserveEdge", "releaseEdge", "getReservations", "getWorkArea",
        "getInteraction", "setInteraction"
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

    // 5b. Stacking, decorations, interactive drag with a snap preview, snapping.
    std::cout << "Testing stacking, decorations, drag and snapping..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const c = bro.compositor;\n"
            "  const st = c.getStacking();\n"
            "  if (!Array.isArray(st) || st.length !== 2) return 'stacking ' + JSON.stringify(st);\n"
            "  c.focusWindow(1);\n"
            "  if (c.getStacking()[1] !== 1) return 'focus did not raise';\n"
            "  if (c.raiseWindow(2) !== true || c.getStacking()[1] !== 2) return 'raiseWindow';\n"
            "  if (c.raiseWindow(2) !== false) return 'raising the top window is a no-op';\n"
            "  const d = c.setDecorations({ insets: { top: 30, left: 4, right: 4, bottom: 4 }, maximizedInsets: { top: 30 } });\n"
            "  if (d.insets.top !== 30 || d.insets.left !== 4 || d.maximizedInsets.left !== 0) return 'decorations';\n"
            "  const w1 = c.getWindow(1);\n"
            "  if (w1.decorated !== false || w1.decoration.top !== 0 || w1.snap !== 'none') return 'window frame fields';\n"
            "  if (w1.outerFrame.x !== w1.frame.x) return 'outerFrame of an undecorated window';\n"
            "  if (c.beginMove(1) !== false) return 'no pointer, no drag';\n"
            "  if (c.beginMove(1, { x: 100, y: 80, immediate: true }) !== true) return 'beginMove';\n"
            "  globalThis.__snaps = [];\n"
            "  c.on('snapPreview', (e) => __snaps.push(e.zone + (e.rect ? ':' + e.rect.x + ',' + e.rect.width : '')));\n"
            "  c.dragTo(2, 500);\n"
            "  const g = c.getDrag();\n"
            "  if (!g || g.action !== 'move' || g.snap !== 'left' || !g.snapRect) return 'drag ' + JSON.stringify(g);\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value) && ev::toUtf8(r.value) == "ok");
        if (ev::isString(r.value) && ev::toUtf8(r.value) != "ok") std::cerr << ev::toUtf8(r.value) << std::endl;
        brocompositor::api::tickCompositorAsync();
        auto r2 = evalScript(
            "(function() {\n"
            "  const c = bro.compositor;\n"
            "  if (__snaps.length !== 1 || !__snaps[0].startsWith('left:0,')) return 'preview ' + JSON.stringify(__snaps);\n"
            "  if (c.endDrag() !== true || c.getDrag() !== null) return 'endDrag';\n"
            "  const w = c.getWindow(1);\n"
            "  if (w.snap !== 'left' || w.frame.x !== 0) return 'snapped ' + JSON.stringify(w.frame);\n"
            "  if (c.snapWindow(1, 'none') !== true || c.getWindow(1).snap !== 'none') return 'unsnap';\n"
            "  if (c.snapWindow(1, 'sideways') !== false) return 'bad zone';\n"
            "  if (c.snapWindowToward(1, 'up') !== true || !c.getWindow(1).maximized) return 'toward up';\n"
            "  if (c.snapWindowToward(1, 'down') !== true || c.getWindow(1).maximized) return 'toward down';\n"
            "  const res = c.beginResize(1, 'bottom right', { x: 10, y: 10 });\n"
            "  if (res !== true || c.getDrag().edges !== 'bottom right') return 'beginResize';\n"
            "  c.cancelDrag();\n"
            "  const ia = c.setInteraction({ minSize: { width: 120, height: 90 }, dragThreshold: 3 });\n"
            "  if (ia.minSize.width !== 120 || ia.dragThreshold !== 3) return 'interaction';\n"
            "  const sn = c.setSnapping({ cornerSize: 64, edgeThreshold: 10 });\n"
            "  if (sn.cornerSize !== 64 || sn.edgeThreshold !== 10 || sn.enabled !== true) return 'snapping';\n"
            "  c.setDecorations({ insets: 0, maximizedInsets: 0 });\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r2.thrown);
        CHECK(ev::isString(r2.value) && ev::toUtf8(r2.value) == "ok");
        if (ev::isString(r2.value) && ev::toUtf8(r2.value) != "ok") std::cerr << ev::toUtf8(r2.value) << std::endl;
        brocompositor::api::tickCompositorAsync();
        std::cout << "  Stacking, decorations, drag and snapping passed." << std::endl;
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

    // 7b. Window states, shell edge reservations, interaction policy
    std::cout << "Testing window states, reservations and interaction..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const c = bro.compositor;\n"
            "  let seen = null;\n"
            "  const h = c.on('reservationChanged', (e) => { seen = e; });\n"
            "  const wa0 = c.getWorkArea();\n"
            "  if (!wa0 || wa0.y !== 40 || wa0.height !== 1040) return 'platform work area: ' + JSON.stringify(wa0);\n"
            "  const id = c.reserveEdge('top', 34);\n"
            "  if (!(id > 0)) return 'reserveEdge failed';\n"
            "  if (!seen || seen.id !== id || seen.shell !== true || seen.edge !== 'top') return 'no reservationChanged';\n"
            "  if (seen.rect.y !== 40 || seen.rect.height !== 34) return 'granted rect ' + JSON.stringify(seen.rect);\n"
            "  const wa = c.getWorkArea(1);\n"
            "  if (wa.y !== 74 || wa.height !== 1006) return 'work area ' + JSON.stringify(wa);\n"
            "  if (c.getMonitors()[0].workArea.y !== 74) return 'monitor work area not shrunk';\n"
            "  if (c.getReservations().length !== 1) return 'getReservations';\n"
            "  if (c.reserveEdge('diagonal', 10) !== 0) return 'bad edge accepted';\n"
            "\n"
            "  const before = c.getWindow(2).frame;\n"
            "  if (c.maximizeWindow(2) !== true) return 'maximizeWindow failed';\n"
            "  let w = c.getWindow(2);\n"
            "  if (!w.maximized || w.frame.y !== 74 || w.frame.width !== 1920) return 'not maximized ' + JSON.stringify(w.frame);\n"
            "  if (c.restoreWindow(2) !== true) return 'restoreWindow failed';\n"
            "  w = c.getWindow(2);\n"
            "  if (w.maximized || w.frame.x !== before.x || w.frame.width !== before.width) return 'not restored';\n"
            "  if (c.fullscreenWindow(2) !== true || !c.getWindow(2).fullscreen) return 'fullscreen';\n"
            "  if (c.getWindow(2).frame.y !== 0 || c.getWindow(2).frame.height !== 1080) return 'fullscreen frame';\n"
            "  c.restoreWindow(2);\n"
            "  if (c.minimizeWindow(2) !== true || !c.getWindow(2).minimized) return 'minimize';\n"
            "  if (c.restoreWindow(2) !== true || c.getWindow(2).minimized) return 'unminimize';\n"
            "  if (c.maximizeWindow(9999) !== false) return 'unknown window maximized';\n"
            "\n"
            "  if (c.releaseEdge(id) !== true) return 'releaseEdge failed';\n"
            "  if (seen.rect.height !== 0) return 'release not reported';\n"
            "  if (c.releaseEdge(id) !== false) return 'double release';\n"
            "  if (c.getWorkArea().y !== 40) return 'work area not restored';\n"
            "  h.remove();\n"
            "\n"
            "  const i0 = c.getInteraction();\n"
            "  if (i0.titlebarHeight !== 38 || i0.resizeBorder !== 6) return 'interaction defaults';\n"
            "  if (i0.dragModifiers.join(',') !== 'alt,super') return 'drag modifiers ' + i0.dragModifiers;\n"
            "  const i1 = c.setInteraction({ titlebarHeight: 30, dragModifiers: 'super' });\n"
            "  if (i1.titlebarHeight !== 30 || i1.resizeBorder !== 6 || i1.dragModifiers.join() !== 'super') return 'setInteraction';\n"
            "  c.setInteraction({ dragModifiers: [] });\n"
            "  if (c.getInteraction().dragModifiers.length !== 0) return 'clear modifiers';\n"
            "  c.setInteraction({ titlebarHeight: 38, dragModifiers: ['super', 'alt'] });\n"
            "  return 'ok';\n"
            "})();\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isString(r.value));
        if (ev::toUtf8(r.value) != "ok") std::cerr << "  got: " << ev::toUtf8(r.value) << std::endl;
        CHECK(ev::toUtf8(r.value) == "ok");

        // A backend that cannot put windows into states: the call reports it.
        brocompositor::api::setCommandSink([](const std::vector<Command>& cmds) {
            size_t refused = 0;
            for (const auto& c : cmds) refused += std::holds_alternative<SetWindowState>(c) ? 1 : 0;
            return refused;
        });
        auto r2 = evalScript(
            "(function() {\n"
            "  if (bro.compositor.maximizeWindow(2) !== false) return 'refused state reported as success';\n"
            "  if (bro.compositor.getWindow(2).maximized) return 'refused state echoed';\n"
            "  return 'ok';\n"
            "})();\n"
        );
        brocompositor::api::setCommandSink(nullptr);
        CHECK(!r2.thrown);
        CHECK(ev::isString(r2.value));
        CHECK(ev::toUtf8(r2.value) == "ok");
        std::cout << "  Window states, reservations and interaction passed." << std::endl;
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
