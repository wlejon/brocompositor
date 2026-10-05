// bc_mac_test_app: a regular AppKit application that owns the windows the
// macOS backend tests manage, so every test exercises the real cross-process
// paths and never touches a window it did not create.
//
// Line protocol on stdin (one reply line on stdout per command):
//   create <name> <x> <y> <w> <h> <rrggbb>   -> ok <CGWindowID>   (frame in Quartz points)
//   move <name> <x> <y> <w> <h>              -> ok
//   frame <name>                             -> ok <x> <y> <w> <h>
//   title <name> <text...>                   -> ok
//   minimize <name> / unminimize <name>      -> ok
//   activate <name>                          -> ok   (makes this app frontmost, the window key)
//   close <name>                             -> ok
//   yield <pid>                              -> ok   (re-activates the app that was frontmost)
//   hang <ms>                                -> ok, then the main thread sleeps <ms>
//   quit                                     -> ok, exits
#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::map<std::string, NSWindow*> g_windows;

void reply(const std::string& s) {
    std::fputs((s + "\n").c_str(), stdout);
    std::fflush(stdout);
}

CGFloat primary_height() { return NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 0; }

// Quartz (top-left origin, y down) <-> Cocoa (bottom-left origin, y up).
NSRect to_cocoa(double x, double y, double w, double h) { return NSMakeRect(x, primary_height() - y - h, w, h); }

NSColor* color(const std::string& hex) {
    unsigned v = unsigned(std::strtoul(hex.c_str(), nullptr, 16));
    return [NSColor colorWithSRGBRed:((v >> 16) & 255) / 255.0
                               green:((v >> 8) & 255) / 255.0
                                blue:(v & 255) / 255.0
                               alpha:1.0];
}

std::string run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, name;
    in >> cmd;
    if (cmd == "quit") {
        reply("ok");
        std::exit(0);
    }
    if (cmd == "yield") {
        // Hands the frontmost position back to the application that had it
        // before a test activated this one.
        int pid = 0;
        in >> pid;
        NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        if (!a) return "error no-such-app";
        if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:a];
        return [a activateWithOptions:0] ? "ok" : "error refused";
    }
    if (cmd == "hang") {
        int ms = 0;
        in >> ms;
        reply("ok");
        usleep(useconds_t(ms) * 1000);
        return {};
    }
    in >> name;
    if (cmd == "create") {
        double x, y, w, h;
        std::string hex;
        in >> x >> y >> w >> h >> hex;
        NSWindow* win = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, w, h)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        win.releasedWhenClosed = NO;
        win.title = [NSString stringWithUTF8String:name.c_str()];
        win.backgroundColor = color(hex);
        [win setFrame:to_cocoa(x, y, w, h) display:YES];
        [win orderFrontRegardless];
        g_windows[name] = win;
        return "ok " + std::to_string(long(win.windowNumber));
    }
    auto it = g_windows.find(name);
    if (it == g_windows.end()) return "error no-such-window";
    NSWindow* win = it->second;
    if (cmd == "move") {
        double x, y, w, h;
        in >> x >> y >> w >> h;
        [win setFrame:to_cocoa(x, y, w, h) display:YES];
        return "ok";
    }
    if (cmd == "frame") {
        NSRect f = win.frame;
        std::ostringstream o;
        o << "ok " << f.origin.x << " " << primary_height() - f.origin.y - f.size.height << " " << f.size.width << " "
          << f.size.height;
        return o.str();
    }
    if (cmd == "title") {
        std::string text;
        std::getline(in, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        win.title = [NSString stringWithUTF8String:text.c_str()];
        return "ok";
    }
    if (cmd == "minimize") {
        [win miniaturize:nil];
        return "ok";
    }
    if (cmd == "unminimize") {
        [win deminiaturize:nil];
        return "ok";
    }
    if (cmd == "activate") {
        [NSApp activateIgnoringOtherApps:YES];
        [win makeKeyAndOrderFront:nil];
        return "ok";
    }
    if (cmd == "close") {
        [win close];
        g_windows.erase(it);
        return "ok";
    }
    return "error unknown-command";
}

}  // namespace

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        std::thread([] {
            std::string line;
            while (std::getline(std::cin, line)) {
                dispatch_sync(dispatch_get_main_queue(), ^{
                  std::string r = run(line);
                  if (!r.empty()) reply(r);
                });
            }
            dispatch_async(dispatch_get_main_queue(), ^{
              std::exit(0);
            });
        }).detach();
        reply("ready");
        [NSApp run];
    }
    return 0;
}
