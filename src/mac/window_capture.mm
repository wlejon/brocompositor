// ScreenCaptureKit stream of one window into a SurfaceRing.
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "brocompositor/mac/shell_backend.h"  // query_permissions
#include "mac/ax.h"
#include "mac/metal_impl.h"
#include "mac/system.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace bcm = brocompositor::mac;

namespace brocompositor::mac {

// ScreenCaptureKit cannot tell a closed window from one that is ordered out:
// both fade out, then the stream reports SCFrameStatusSuspended and keeps
// running, and the window server keeps the closed window (off screen, same
// id) for as long as its process lives. Accessibility can: the window's AX
// element turns invalid when it is closed or ordered out, and stays valid
// while it is minimized or on another Space (the shell backend's
// WindowRemoved rule). With the permission, a suspended stream asks it.
// Every AX call runs on `queue` (an AX call blocks for up to kTimeout on a
// busy application); `element` is touched only there.
struct AxWatch {
    static constexpr std::chrono::milliseconds kTimeout{250};
    dispatch_queue_t queue = nil;
    uint32_t window = 0;
    ax::Element app;
    ax::Element element;  // the window's element (re-created by AX on deminiaturize)
    std::atomic<bool>* closed = nullptr;
    std::atomic<bool> busy{false};  // a check is queued or running

    // Finds the window's current element among the application's windows.
    bool lookup(AXError* error = nullptr) {
        AXError err = kAXErrorSuccess;
        for (auto& w : ax::windows(app.get(), kTimeout, &err))
            if (ax::window_id(w.get()) == window) {
                element = std::move(w);
                err = kAXErrorSuccess;
                if (error) *error = err;
                return true;
            }
        if (error) *error = err;
        return false;
    }
    // The stream is suspended: was the window closed (or ordered out)?
    void check() {
        if (!element) {
            lookup();  // never found yet: nothing to compare against
            return;
        }
        AXError err = ax::probe(element.get());
        if (!ax::gone(err)) return;  // valid (minimized, another Space), or the app is busy
        if (lookup(&err)) return;    // a re-created element (deminiaturized)
        if (err != kAXErrorSuccess && err != kAXErrorNoValue) return;  // the application did not answer
        sys::trace("capture %u: AX element gone: closed or ordered out", window);
        closed->store(true);
    }
};

struct CaptureSink {
    std::mutex mutex;
    SurfaceRing* ring = nullptr;  // null once the capture is being torn down
    std::atomic<bool> closed{false};
    std::atomic<bool> suspended{false};  // SCFrameStatusSuspended: the window is not displayed
    AxWatch* ax = nullptr;               // with Accessibility
    uint32_t window = 0;
    double scale = 2.0;
    size_t out_w = 0, out_h = 0;
    std::chrono::steady_clock::time_point next_size_check{};
    long last_status = -1;  // the last SCFrameStatus seen (capture queue only)
};

}  // namespace brocompositor::mac

API_AVAILABLE(macos(12.3))
@interface BCStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) bcm::CaptureSink* sink;
@property(nonatomic, weak) SCStream* stream;
@property(nonatomic, strong) SCStreamConfiguration* config;
@end

@implementation BCStreamOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)buffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) return;
    bcm::CaptureSink* sink = self.sink;
    if (!sink) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (status && status.integerValue != sink->last_status) {
        sink->last_status = status.integerValue;
        bcm::sys::trace("capture %u: frame status %ld", sink->window, long(status.integerValue));
        if (status.integerValue == SCFrameStatusStopped) sink->closed = true;
        if (status.integerValue == SCFrameStatusSuspended) sink->suspended = true;
        if (status.integerValue == SCFrameStatusComplete && sink->suspended.exchange(false) && sink->ax) {
            // Displayed again: AX may have re-created the window's element
            // (it does on deminiaturize). Pick up the current one.
            bcm::AxWatch* ax = sink->ax;
            dispatch_async(ax->queue, ^{
              ax->lookup();
            });
        }
    }
    if (!status || status.integerValue != SCFrameStatusComplete) return;
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(buffer);
    IOSurfaceRef surface = pixels ? CVPixelBufferGetIOSurface(pixels) : nullptr;
    if (!surface) return;

    brocompositor::Size content{int32_t(IOSurfaceGetWidth(surface)), int32_t(IOSurfaceGetHeight(surface))};
    NSDictionary* rect = info[SCStreamFrameInfoContentRect];
    NSNumber* scale = info[SCStreamFrameInfoScaleFactor];
    CGRect r{};
    if (rect && scale && CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)rect, &r)) {
        content.width = std::min(content.width, int32_t(std::lround(r.size.width * scale.doubleValue)));
        content.height = std::min(content.height, int32_t(std::lround(r.size.height * scale.doubleValue)));
    }
    int64_t ns = 0;
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(buffer);
    if (CMTIME_IS_VALID(pts)) ns = int64_t(CMTimeGetSeconds(pts) * 1e9);

    std::lock_guard<std::mutex> lock(sink->mutex);
    if (!sink->ring) return;
    sink->ring->submit(surface, content, ns);

    // Follow the window's size (ScreenCaptureKit otherwise scales it into
    // the configured output).
    auto now = std::chrono::steady_clock::now();
    if (now < sink->next_size_check) return;
    sink->next_size_check = now + std::chrono::milliseconds(250);
    auto w = bcm::sys::describe_window(sink->window);
    if (!w || w->frame.empty()) return;
    size_t want_w = size_t(std::lround(w->frame.width * sink->scale));
    size_t want_h = size_t(std::lround(w->frame.height * sink->scale));
    if (want_w == sink->out_w && want_h == sink->out_h) return;
    sink->out_w = want_w;
    sink->out_h = want_h;
    self.config.width = want_w;
    self.config.height = want_h;
    [self.stream updateConfiguration:self.config completionHandler:^(NSError*){}];
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    bcm::sys::trace("capture: stream stopped: %ld %s", long(error.code),
                    error ? error.localizedDescription.UTF8String : "");
    if (bcm::CaptureSink* sink = self.sink) sink->closed = true;
}

@end

namespace brocompositor::mac {

struct WindowCapture::Impl {
    std::unique_ptr<SurfaceRing> ring;
    CaptureSink sink;
    SCStream* stream API_AVAILABLE(macos(12.3)) = nil;
    BCStreamOutput* output API_AVAILABLE(macos(12.3)) = nil;
    dispatch_queue_t queue = nil;
    std::unique_ptr<AxWatch> ax;  // with Accessibility
    mutable std::atomic<int64_t> last_exists_check{0};  // steady ms

    // After the capture queue drained (~WindowCapture): nothing submits to
    // the AX queue any more; wait out a running check.
    ~Impl() {
        if (ax) dispatch_sync(ax->queue, ^{});
    }
};

namespace {

// Runs an asynchronous ScreenCaptureKit call to completion (bounded).
bool wait(dispatch_semaphore_t s, double seconds) {
    return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, int64_t(seconds * NSEC_PER_SEC))) == 0;
}

}  // namespace

WindowCapture::WindowCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

bool capture_supported() {
    if (@available(macOS 12.3, *)) return CGPreflightScreenCaptureAccess();
    return false;
}

std::unique_ptr<WindowCapture> WindowCapture::start(std::shared_ptr<MetalDevice> device, uint64_t window,
                                                    const CaptureConfig& config, std::string* error) {
    auto fail = [&](std::string msg) -> std::unique_ptr<WindowCapture> {
        if (error) *error = std::move(msg);
        return nullptr;
    };
    if (@available(macOS 12.3, *)) {
    } else {
        return fail("ScreenCaptureKit needs macOS 12.3");
    }
    if (!device) return fail("no Metal device");
    // Never prompt: CGRequestScreenCaptureAccess / SCShareableContent may
    // (for an application; tccd never prompts for a platform binary such as
    // sshd-keygen-wrapper and denies instead). Both read the same TCC grant
    // (kTCCServiceScreenCapture); tccd logs the same subject for each.
    auto refused = [&] {
        return fail("Screen Recording permission not granted to " + query_permissions().responsible_path +
                    " (System Settings > Privacy & Security > Screen & System Audio Recording)");
    };
    if (!CGPreflightScreenCaptureAccess()) return refused();
    auto impl = std::make_unique<Impl>();
    RingConfig rc;
    rc.ring_size = config.ring_size;
    impl->ring = SurfaceRing::create(device, rc, error);
    if (!impl->ring) return nullptr;
    impl->sink.window = uint32_t(window);

    @autoreleasepool {
        __block SCShareableContent* content = nil;
        __block NSError* content_error = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                                   onScreenWindowsOnly:NO
                                                     completionHandler:^(SCShareableContent* c, NSError* e) {
                                                       content = c;
                                                       content_error = e;
                                                       dispatch_semaphore_signal(done);
                                                     }];
        if (!wait(done, 5.0)) return fail("SCShareableContent timed out");
        // SCStreamErrorUserDeclined: TCC said no after all.
        if (!content && content_error && content_error.code == -3801) return refused();
        if (!content)
            return fail(std::string("SCShareableContent failed: ") +
                        (content_error ? content_error.localizedDescription.UTF8String : "unknown"));
        SCWindow* target = nil;
        for (SCWindow* w in content.windows)
            if (w.windowID == CGWindowID(window)) target = w;
        if (!target) return fail("window is not shareable (gone, or not a window-server window)");

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
        double scale = 2.0;
        if (@available(macOS 14.0, *)) scale = filter.pointPixelScale;
        else scale = NSScreen.mainScreen.backingScaleFactor;
        SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
        impl->sink.scale = scale;
        impl->sink.out_w = size_t(std::lround(target.frame.size.width * scale));
        impl->sink.out_h = size_t(std::lround(target.frame.size.height * scale));
        cfg.width = std::max<size_t>(impl->sink.out_w, 1);
        cfg.height = std::max<size_t>(impl->sink.out_h, 1);
        cfg.pixelFormat = 'BGRA';
        // sRGB, whatever the display's colour space (by default the stream
        // delivers the display's: on a P3 panel sRGB 20B040 arrives as
        // 54AD4F).
        cfg.colorSpaceName = kCGColorSpaceSRGB;
        cfg.showsCursor = config.capture_cursor;
        cfg.minimumFrameInterval = CMTimeMake(1, int32_t(std::max(1u, config.max_fps)));
        cfg.queueDepth = 3;

        if (AXIsProcessTrusted() && target.owningApplication) {
            impl->ax = std::make_unique<AxWatch>();
            impl->ax->queue = dispatch_queue_create("brocompositor.capture.ax", DISPATCH_QUEUE_SERIAL);
            impl->ax->window = uint32_t(window);
            impl->ax->app = ax::application(uint32_t(target.owningApplication.processID), AxWatch::kTimeout);
            impl->ax->closed = &impl->sink.closed;
            impl->sink.ax = impl->ax.get();
            AxWatch* watch = impl->ax.get();
            dispatch_async(watch->queue, ^{
              watch->lookup();
            });
        }
        impl->queue = dispatch_queue_create("brocompositor.capture", DISPATCH_QUEUE_SERIAL);
        impl->output = [[BCStreamOutput alloc] init];
        impl->output.sink = &impl->sink;
        impl->output.config = cfg;
        {
            std::lock_guard<std::mutex> lock(impl->sink.mutex);
            impl->sink.ring = impl->ring.get();
        }
        impl->stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:impl->output];
        impl->output.stream = impl->stream;
        NSError* add_error = nil;
        if (![impl->stream addStreamOutput:impl->output
                                      type:SCStreamOutputTypeScreen
                        sampleHandlerQueue:impl->queue
                                     error:&add_error])
            return fail(std::string("addStreamOutput failed: ") +
                        (add_error ? add_error.localizedDescription.UTF8String : "unknown"));
        __block NSError* start_error = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [impl->stream startCaptureWithCompletionHandler:^(NSError* e) {
          start_error = e;
          dispatch_semaphore_signal(started);
        }];
        if (!wait(started, 5.0)) return fail("SCStream start timed out");
        if (start_error) return fail(std::string("SCStream start failed: ") + start_error.localizedDescription.UTF8String);
    }
    return std::unique_ptr<WindowCapture>(new WindowCapture(std::move(impl)));
}

WindowCapture::~WindowCapture() {
    if (@available(macOS 12.3, *)) {
        @autoreleasepool {
            if (impl_->stream) {
                dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
                [impl_->stream stopCaptureWithCompletionHandler:^(NSError*) {
                  dispatch_semaphore_signal(stopped);
                }];
                wait(stopped, 2.0);
                [impl_->stream removeStreamOutput:impl_->output type:SCStreamOutputTypeScreen error:nil];
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->sink.mutex);
        impl_->sink.ring = nullptr;
    }
    if (@available(macOS 12.3, *)) impl_->output.sink = nullptr;
    if (impl_->queue) dispatch_sync(impl_->queue, ^{});  // wait out an in-flight frame
}

std::optional<Frame> WindowCapture::acquire() { return impl_->ring->acquire(); }
void WindowCapture::release(const Frame& frame) { impl_->ring->release(frame); }
std::vector<SharedImage> WindowCapture::images() const { return impl_->ring->images(); }
std::optional<SharedImage> WindowCapture::image(uint64_t id) const { return impl_->ring->image(id); }
uint64_t WindowCapture::images_generation() const { return impl_->ring->images_generation(); }
SharedTimeline WindowCapture::timeline() const { return impl_->ring->timeline(); }
bool WindowCapture::closed() const {
    if (impl_->sink.closed.load() || impl_->ring->closed()) return true;
    // A window that is gone delivers no frames, and whether the stream then
    // stops with an error is ScreenCaptureKit's business: ask the window
    // server (at most every 250 ms).
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t last = impl_->last_exists_check.load();
    if (now - last >= 250 && impl_->last_exists_check.compare_exchange_strong(last, now)) {
        if (!sys::describe_window(impl_->sink.window)) {
            sys::trace("capture %u: window gone from the window server", impl_->sink.window);
            impl_->sink.closed = true;
        }
        // Not displayed: closed, ordered out, minimized or on another Space.
        // Accessibility tells the first two from the others (off this
        // thread: an AX call can block on a busy application).
        AxWatch* ax = impl_->ax.get();
        if (ax && impl_->sink.suspended.load() && !ax->busy.exchange(true)) {
            dispatch_async(ax->queue, ^{
              ax->check();
              ax->busy = false;
            });
        }
    }
    return impl_->sink.closed.load();
}
void WindowCapture::set_frame_callback(std::function<void()> callback) {
    impl_->ring->set_frame_callback(std::move(callback));
}
uint64_t WindowCapture::frames_dropped() const { return impl_->ring->frames_dropped(); }

}  // namespace brocompositor::mac
