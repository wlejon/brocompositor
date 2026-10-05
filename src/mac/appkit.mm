// AppKit half of mac/system.h: displays, running applications, workspace
// notifications. Everything else in the backend is plain C++.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include "mac/system.h"

#include <cmath>

namespace brocompositor::mac::sys {

namespace {

Rect to_rect(CGRect r) {
    int32_t x = int32_t(std::lround(r.origin.x)), y = int32_t(std::lround(r.origin.y));
    return Rect{x, y, int32_t(std::lround(r.origin.x + r.size.width)) - x,
                int32_t(std::lround(r.origin.y + r.size.height)) - y};
}

std::string str(NSString* s) {
    const char* p = s ? s.UTF8String : nullptr;
    return p ? std::string(p) : std::string();
}

}  // namespace

std::vector<Screen> screens() {
    std::vector<Screen> out;
    @autoreleasepool {
        NSArray<NSScreen*>* all = [NSScreen screens];
        if (all.count == 0) return out;
        // Cocoa's global space has its origin at the bottom-left of the
        // primary screen (the first one), y up; Quartz flips it.
        CGFloat primary_height = all[0].frame.size.height;
        for (NSScreen* s in all) {
            NSNumber* number = s.deviceDescription[@"NSScreenNumber"];
            if (!number) continue;
            CGDirectDisplayID id = number.unsignedIntValue;
            if (CGDisplayIsAsleep(id)) continue;
            Screen sc;
            sc.display_id = id;
            if (@available(macOS 10.15, *)) sc.name = str(s.localizedName);
            if (sc.name.empty()) sc.name = "display-" + std::to_string(id);
            sc.frame = to_rect(CGDisplayBounds(id));
            NSRect v = s.visibleFrame;
            sc.visible = to_rect(CGRectMake(v.origin.x, primary_height - v.origin.y - v.size.height, v.size.width,
                                            v.size.height));
            sc.scale = s.backingScaleFactor;
            sc.primary = CGDisplayIsMain(id);
            out.push_back(sc);
        }
    }
    return out;
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
    // The Process Manager asks the system each time; NSWorkspace's property
    // is refreshed by notifications on the main run loop, which a host may
    // not run. Deprecated but present on every macOS this targets.
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
