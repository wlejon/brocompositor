// bc_x11_client: a scripted X11 client (plain xcb) for the XWayland tests.
// It maps a window with the ICCCM / EWMH properties a window manager reads
// and prints what the window manager does to it, one line per event:
//
//   mapped <id>                 the main window is viewable
//   configure <x> <y> <w> <h>   ConfigureNotify of the main window
//   focus_in / focus_out
//   key <keycode> <0|1>         KeyPress / KeyRelease (X keycode = evdev + 8)
//   button <b> <0|1>
//   delete                      WM_DELETE_WINDOW (the client then exits 0)
//   state <atoms...>            _NET_WM_STATE changed (fullscreen, maximized_vert, ...)
//
// Options: --class C --instance I --title T --size WxH --pos X,Y --color RRGGBB
//          --transient (a second window, transient for the first, class C,
//          title "<T> dialog") --override X,Y,WxH (an override-redirect window)
//          --undecorated (_MOTIF_WM_HINTS: no window-manager frame)
// stdin:   fullscreen | unfullscreen | maximize | activate | quit
#include <xcb/xcb.h>

#include <poll.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

xcb_connection_t* conn = nullptr;
xcb_screen_t* screen = nullptr;

xcb_atom_t atom(const char* name) {
    auto c = xcb_intern_atom(conn, 0, uint16_t(std::strlen(name)), name);
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(conn, c, nullptr);
    xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
    std::free(r);
    return a;
}

std::string atom_name(xcb_atom_t a) {
    auto c = xcb_get_atom_name(conn, a);
    xcb_get_atom_name_reply_t* r = xcb_get_atom_name_reply(conn, c, nullptr);
    std::string s = r ? std::string(xcb_get_atom_name_name(r), size_t(xcb_get_atom_name_name_length(r))) : "";
    std::free(r);
    return s;
}

void set_string(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, const std::string& s) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, prop, type, 8, uint32_t(s.size()), s.data());
}

struct Atoms {
    xcb_atom_t protocols, delete_window, net_wm_pid, net_wm_name, utf8, net_wm_state, fullscreen, max_v, max_h,
        net_active_window, net_wm_window_type, type_dialog;
} A;

xcb_window_t make_window(int x, int y, int w, int h, uint32_t color, bool override_redirect) {
    xcb_window_t win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[] = {color, override_redirect ? 1u : 0u,
                         XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_KEY_PRESS |
                             XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |
                             XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_PROPERTY_CHANGE |
                             XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, int16_t(x), int16_t(y), uint16_t(w),
                      uint16_t(h), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    return win;
}

void decorate(xcb_window_t w, const std::string& instance, const std::string& cls, const std::string& title) {
    std::string wm_class = instance + '\0' + cls + '\0';
    set_string(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, wm_class);
    set_string(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, title);
    set_string(w, A.net_wm_name, A.utf8, title);
    uint32_t pid = uint32_t(getpid());
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, A.net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, A.protocols, XCB_ATOM_ATOM, 32, 1, &A.delete_window);
}

void send_wm_state(xcb_window_t w, uint32_t action, xcb_atom_t a1, xcb_atom_t a2) {
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = w;
    ev.type = A.net_wm_state;
    ev.data.data32[0] = action;  // 0 remove, 1 add
    ev.data.data32[1] = a1;
    ev.data.data32[2] = a2;
    ev.data.data32[3] = 1;
    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&ev));
}

void send_activate(xcb_window_t w) {
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = w;
    ev.type = A.net_active_window;
    ev.data.data32[0] = 1;  // source: application
    xcb_send_event(conn, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&ev));
}

void print_state(xcb_window_t w) {
    auto c = xcb_get_property(conn, 0, w, A.net_wm_state, XCB_ATOM_ATOM, 0, 32);
    xcb_get_property_reply_t* r = xcb_get_property_reply(conn, c, nullptr);
    std::string line = "state";
    if (r) {
        auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(r));
        int n = xcb_get_property_value_length(r) / 4;
        for (int i = 0; i < n; ++i) {
            if (atoms[i] == A.fullscreen) line += " fullscreen";
            else if (atoms[i] == A.max_v) line += " maximized_vert";
            else if (atoms[i] == A.max_h) line += " maximized_horz";
            else line += " " + atom_name(atoms[i]);
        }
    }
    std::free(r);
    out("%s", line.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    std::string cls = "BcX11", instance = "bcx11", title = "bc-x11";
    int w = 200, h = 150, x = 0, y = 0;
    uint32_t color = 0xC03080;
    bool transient = false;
    bool undecorated = false;
    int ox = -1, oy = -1, ow = 0, oh = 0;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--class") cls = next();
        else if (k == "--instance") instance = next();
        else if (k == "--title") title = next();
        else if (k == "--size") std::sscanf(next().c_str(), "%dx%d", &w, &h);
        else if (k == "--pos") std::sscanf(next().c_str(), "%d,%d", &x, &y);
        else if (k == "--color") color = uint32_t(std::strtoul(next().c_str(), nullptr, 16));
        else if (k == "--transient") transient = true;
        else if (k == "--undecorated") undecorated = true;
        else if (k == "--override") std::sscanf(next().c_str(), "%d,%d,%dx%d", &ox, &oy, &ow, &oh);
    }
    conn = xcb_connect(nullptr, nullptr);
    if (!conn || xcb_connection_has_error(conn)) {
        out("error connect");
        return 2;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    A.protocols = atom("WM_PROTOCOLS");
    A.delete_window = atom("WM_DELETE_WINDOW");
    A.net_wm_pid = atom("_NET_WM_PID");
    A.net_wm_name = atom("_NET_WM_NAME");
    A.utf8 = atom("UTF8_STRING");
    A.net_wm_state = atom("_NET_WM_STATE");
    A.fullscreen = atom("_NET_WM_STATE_FULLSCREEN");
    A.max_v = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    A.max_h = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    A.net_active_window = atom("_NET_ACTIVE_WINDOW");
    A.net_wm_window_type = atom("_NET_WM_WINDOW_TYPE");
    A.type_dialog = atom("_NET_WM_WINDOW_TYPE_DIALOG");

    xcb_window_t main_win = make_window(x, y, w, h, color, false);
    decorate(main_win, instance, cls, title);
    if (undecorated) {
        // flags = MWM_HINTS_DECORATIONS, decorations = 0 (what GTK CSD sets).
        const uint32_t hints[5] = {2, 0, 0, 0, 0};
        xcb_atom_t motif = atom("_MOTIF_WM_HINTS");
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, main_win, motif, motif, 32, 5, hints);
    }
    xcb_map_window(conn, main_win);
    std::vector<xcb_window_t> extra;
    if (transient) {
        xcb_window_t d = make_window(x + 20, y + 20, w / 2, h / 2, 0x30C080, false);
        decorate(d, instance, cls, title + " dialog");
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, d, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1,
                            &main_win);
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, d, A.net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
                            &A.type_dialog);
        xcb_map_window(conn, d);
        extra.push_back(d);
    }
    if (ow > 0) {
        xcb_window_t o = make_window(ox, oy, ow, oh, 0xE0E000, true);
        xcb_map_window(conn, o);
        extra.push_back(o);
    }
    xcb_flush(conn);

    bool mapped = false;
    std::string pending;
    for (;;) {
        pollfd fds[2] = {{xcb_get_file_descriptor(conn), POLLIN, 0}, {0, POLLIN, 0}};
        poll(fds, 2, 1000);
        while (xcb_generic_event_t* ev = xcb_poll_for_event(conn)) {
            uint8_t type = ev->response_type & 0x7f;
            switch (type) {
                case XCB_MAP_NOTIFY: {
                    auto* e = reinterpret_cast<xcb_map_notify_event_t*>(ev);
                    if (e->window == main_win && !mapped) {
                        mapped = true;
                        out("mapped %u", main_win);
                    }
                    break;
                }
                case XCB_CONFIGURE_NOTIFY: {
                    auto* e = reinterpret_cast<xcb_configure_notify_event_t*>(ev);
                    if (e->window == main_win) out("configure %d %d %d %d", e->x, e->y, e->width, e->height);
                    break;
                }
                case XCB_FOCUS_IN:
                    if (reinterpret_cast<xcb_focus_in_event_t*>(ev)->event == main_win) out("focus_in");
                    break;
                case XCB_FOCUS_OUT:
                    if (reinterpret_cast<xcb_focus_out_event_t*>(ev)->event == main_win) out("focus_out");
                    break;
                case XCB_KEY_PRESS:
                case XCB_KEY_RELEASE:
                    out("key %u %d", reinterpret_cast<xcb_key_press_event_t*>(ev)->detail, type == XCB_KEY_PRESS);
                    break;
                case XCB_BUTTON_PRESS:
                case XCB_BUTTON_RELEASE:
                    out("button %u %d", reinterpret_cast<xcb_button_press_event_t*>(ev)->detail,
                        type == XCB_BUTTON_PRESS);
                    break;
                case XCB_PROPERTY_NOTIFY: {
                    auto* e = reinterpret_cast<xcb_property_notify_event_t*>(ev);
                    if (e->window == main_win && e->atom == A.net_wm_state) print_state(main_win);
                    break;
                }
                case XCB_CLIENT_MESSAGE: {
                    auto* e = reinterpret_cast<xcb_client_message_event_t*>(ev);
                    if (e->type == A.protocols && e->data.data32[0] == A.delete_window) {
                        out("delete");
                        std::free(ev);
                        xcb_disconnect(conn);
                        return 0;
                    }
                    break;
                }
                default: break;
            }
            std::free(ev);
        }
        if (xcb_connection_has_error(conn)) {
            out("error connection");
            return 1;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[256];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) break;
            pending.append(buf, size_t(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (line == "fullscreen") send_wm_state(main_win, 1, A.fullscreen, 0);
                else if (line == "unfullscreen") send_wm_state(main_win, 0, A.fullscreen, 0);
                else if (line == "maximize") send_wm_state(main_win, 1, A.max_v, A.max_h);
                else if (line == "activate") send_activate(main_win);
                else if (line == "quit") {
                    xcb_disconnect(conn);
                    return 0;
                }
            }
            xcb_flush(conn);
        }
    }
    xcb_disconnect(conn);
    return 0;
}
