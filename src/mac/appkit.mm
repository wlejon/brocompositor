// AppKit half of mac/system.h: displays, running applications, workspace
// notifications. Everything else in the backend is plain C++.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include "mac/system.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace brocompositor::mac::sys {

namespace {

std::string str(NSString* s) {
    const char* p = s ? s.UTF8String : nullptr;
    return p ? std::string(p) : std::string();
}

}  // namespace

// NSScreen, in a process that does not run NSApplication's event loop on its
// main thread, keeps the configuration it first read (measured on macOS 26:
// after a display mode change NSScreen still reported the old size, even
// after spinning the main run loop). So it contributes only what the
// topology lacks, the names and the menu-bar / Dock insets of the visible
// frame, which the shell applies to the current bounds.
std::map<uint32_t, ScreenInsets> screen_insets() {
    std::map<uint32_t, ScreenInsets> insets;
    @autoreleasepool {
        for (NSScreen* s in [NSScreen screens]) {
            NSNumber* number = s.deviceDescription[@"NSScreenNumber"];
            if (!number) continue;
            ScreenInsets in;
            if (@available(macOS 10.15, *)) in.name = str(s.localizedName);
            NSRect f = s.frame, v = s.visibleFrame;
            in.left = std::max(0.0, v.origin.x - f.origin.x);
            in.bottom = std::max(0.0, v.origin.y - f.origin.y);  // Cocoa: y up
            in.right = std::max(0.0, (f.origin.x + f.size.width) - (v.origin.x + v.size.width));
            in.top = std::max(0.0, (f.origin.y + f.size.height) - (v.origin.y + v.size.height));
            insets[number.unsignedIntValue] = in;
        }
    }
    return insets;
}

std::optional<App> app(uint32_t pid) {
    @autoreleasepool {
        NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:pid_t(pid)];
        if (!a || a.terminated) return std::nullopt;
        App out;
        out.pid = pid;
        out.name = str(a.localizedName);
        out.bundle_id = str(a.bundleIdentifier);
        out.executable = str(a.executableURL.lastPathComponent);
        out.regular = a.activationPolicy == NSApplicationActivationPolicyRegular;
        return out;
    }
}

uint32_t frontmost_pid() {
    // Measured on macOS 26 from a process that does not run a main run
    // loop: GetFrontProcess follows activations at once and answers in
    // about a millisecond even while the frontmost application hangs;
    // NSWorkspace.frontmostApplication and NSRunningApplication.active never
    // change (they are refreshed by notifications on the main run loop); the
    // supported AX route (system-wide AXFocusedApplication) is current but
    // is served by the frontmost application and blocks for the whole
    // messaging timeout when it hangs. GetFrontProcess is deprecated, not
    // removed, and the only one of them a tracking thread can rely on;
    // NSWorkspace remains as the fallback should it ever stop answering.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ProcessSerialNumber psn{};
    pid_t pid = 0;
    if (GetFrontProcess(&psn) == noErr && GetProcessPID(&psn, &pid) == noErr && pid > 0) return uint32_t(pid);
#pragma clang diagnostic pop
    @autoreleasepool {
        NSRunningApplication* a = NSWorkspace.sharedWorkspace.frontmostApplication;
        return a ? uint32_t(a.processIdentifier) : 0;
    }
}

uint32_t finder_pid() {
    @autoreleasepool {
        NSArray* apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.finder"];
        return apps.count ? uint32_t([apps[0] processIdentifier]) : 0;
    }
}

bool activate(uint32_t pid) {
    @autoreleasepool {
        NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:pid_t(pid)];
        return a && [a activateWithOptions:NSApplicationActivateAllWindows];
    }
}

struct WorkspaceWatch::Impl {
    NSMutableArray* tokens = [NSMutableArray array];
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
};

WorkspaceWatch::WorkspaceWatch(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<WorkspaceWatch> WorkspaceWatch::start(std::function<void()> changed) {
    auto impl = std::make_unique<Impl>();
    @autoreleasepool {
        impl->queue.maxConcurrentOperationCount = 1;
        auto cb = std::make_shared<std::function<void()>>(std::move(changed));
        void (^block)(NSNotification*) = ^(NSNotification*) {
          (*cb)();
        };
        NSNotificationCenter* ws = NSWorkspace.sharedWorkspace.notificationCenter;
        for (NSNotificationName n in @[
                 NSWorkspaceDidLaunchApplicationNotification, NSWorkspaceDidTerminateApplicationNotification,
                 NSWorkspaceDidActivateApplicationNotification, NSWorkspaceDidHideApplicationNotification,
                 NSWorkspaceDidUnhideApplicationNotification, NSWorkspaceActiveSpaceDidChangeNotification
             ])
            [impl->tokens addObject:[ws addObserverForName:n object:nil queue:impl->queue usingBlock:block]];
        [impl->tokens addObject:[NSNotificationCenter.defaultCenter
                                    addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                object:nil
                                                 queue:impl->queue
                                            usingBlock:block]];
    }
    return std::unique_ptr<WorkspaceWatch>(new WorkspaceWatch(std::move(impl)));
}

WorkspaceWatch::~WorkspaceWatch() {
    @autoreleasepool {
        NSNotificationCenter* ws = NSWorkspace.sharedWorkspace.notificationCenter;
        for (id t in impl_->tokens) {
            [ws removeObserver:t];
            [NSNotificationCenter.defaultCenter removeObserver:t];
        }
        [impl_->queue cancelAllOperations];
        [impl_->queue waitUntilAllOperationsAreFinished];
    }
}

}  // namespace brocompositor::mac::sys
