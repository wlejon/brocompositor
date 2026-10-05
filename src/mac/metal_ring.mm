// MetalDevice and SurfaceRing: IOSurface-backed slots filled by Metal blits,
// ordered by a MTLSharedEvent timeline (see mac/capture.h).
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include "mac/metal_impl.h"

#include <atomic>
#include <mutex>

namespace brocompositor::mac {

namespace {

std::atomic<uint64_t> g_next_id{1};
constexpr OSType kBGRA = 'BGRA';

struct Slot {
    IOSurfaceRef surface = nullptr;
    id<MTLTexture> texture = nil;
    SharedImage desc;
    int leases = 0;
    bool writing = false;
    bool has_frame = false;
    Frame frame;

    ~Slot() {
        texture = nil;
        if (surface) CFRelease(surface);
    }
};

IOSurfaceRef make_surface(uint32_t w, uint32_t h) {
    NSDictionary* props = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(kBGRA),
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

id<MTLTexture> texture_for(id<MTLDevice> dev, IOSurfaceRef surface, MTLTextureUsage usage) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:IOSurfaceGetWidth(surface)
                                                                                 height:IOSurfaceGetHeight(surface)
                                                                              mipmapped:NO];
    d.usage = usage;
    d.storageMode = dev.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    return [dev newTextureWithDescriptor:d iosurface:surface plane:0];
}

}  // namespace

// ---------------------------------------------------------------- MetalDevice

MetalDevice::MetalDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalDevice::~MetalDevice() = default;

std::shared_ptr<MetalDevice> MetalDevice::create(const MetalDeviceConfig& config, std::string* error) {
    auto impl = std::make_unique<Impl>();
    @autoreleasepool {
        if (config.adapter && config.adapter->metal_registry_id) {
            for (id<MTLDevice> d in MTLCopyAllDevices())
                if (d.registryID == config.adapter->metal_registry_id) impl->device = d;
        } else {
            impl->device = MTLCreateSystemDefaultDevice();
        }
        if (!impl->device) {
            if (error) *error = config.adapter ? "no Metal device with that registryID" : "no Metal device";
            return nullptr;
        }
        impl->queue = [impl->device newCommandQueue];
        impl->adapter = adapter_of_registry(impl->device.registryID);
    }
    return std::shared_ptr<MetalDevice>(new MetalDevice(std::move(impl)));
}

AdapterId MetalDevice::adapter() const { return impl_->adapter; }
void* MetalDevice::mtl_device() const { return (__bridge void*)impl_->device; }

// ---------------------------------------------------------------- SurfaceRing

struct SurfaceRing::Impl {
    std::shared_ptr<MetalDevice> device;
    RingConfig config;
    id<MTLSharedEvent> event = nil;
    uint64_t event_id = g_next_id.fetch_add(1);
    uint64_t event_value = 0;  // under submit_mutex

    std::mutex submit_mutex;   // one submit at a time: timeline values commit in order
    mutable std::mutex mutex;  // slots / latest / leases
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::unique_ptr<Slot>> retiring;
    int latest = -1;
    uint32_t width = 0, height = 0;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> is_closed{false};
    std::function<void()> callback;

    bool make_slots(uint32_t w, uint32_t h);
};

bool SurfaceRing::Impl::make_slots(uint32_t w, uint32_t h) {
    // Caller holds `mutex`. Leased images retire until released.
    for (auto& s : slots)
        if (s->leases > 0) retiring.push_back(std::move(s));
    slots.clear();
    latest = -1;
    ++generation;
    width = w;
    height = h;
    auto& dev = device->impl();
    for (uint32_t i = 0; i < std::max(3u, config.ring_size); ++i) {
        auto slot = std::make_unique<Slot>();
        slot->surface = make_surface(w, h);
        if (!slot->surface) return false;
        slot->texture = texture_for(dev.device, slot->surface, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
        if (!slot->texture) return false;
        slot->desc.id = g_next_id.fetch_add(1);
        slot->desc.type = ImageHandleType::IOSurface;
        slot->desc.handle.value = uint64_t(reinterpret_cast<uintptr_t>(slot->surface));
        slot->desc.iosurface_id = IOSurfaceGetID(slot->surface);
        slot->desc.width = w;
        slot->desc.height = h;
        slot->desc.format = PixelFormat::BGRA8Unorm;
        slot->desc.adapter = dev.adapter;
        slots.push_back(std::move(slot));
    }
    return true;
}

SurfaceRing::SurfaceRing(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SurfaceRing::~SurfaceRing() {
    std::lock_guard<std::mutex> serial(impl_->submit_mutex);  // wait out an in-flight submit
}

std::unique_ptr<SurfaceRing> SurfaceRing::create(std::shared_ptr<MetalDevice> device, const RingConfig& config,
                                                 std::string* error) {
    if (!device) {
        if (error) *error = "no Metal device";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->config = config;
    impl->event = [device->impl().device newSharedEvent];
    if (!impl->event) {
        if (error) *error = "newSharedEvent failed";
        return nullptr;
    }
    return std::unique_ptr<SurfaceRing>(new SurfaceRing(std::move(impl)));
}

bool SurfaceRing::submit(void* iosurface, Size content, int64_t timestamp_ns) {
    auto src_surface = static_cast<IOSurfaceRef>(iosurface);
    Impl& d = *impl_;
    if (!src_surface || d.is_closed.load() || IOSurfaceGetPixelFormat(src_surface) != kBGRA) {
        d.dropped.fetch_add(1);
        return false;
    }
    std::lock_guard<std::mutex> serial(d.submit_mutex);
    uint32_t sw = uint32_t(IOSurfaceGetWidth(src_surface)), sh = uint32_t(IOSurfaceGetHeight(src_surface));
    Slot* slot = nullptr;
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if ((sw != d.width || sh != d.height) && !d.make_slots(sw, sh)) {
            d.dropped.fetch_add(1);
            return false;
        }
        for (int i = 0; i < int(d.slots.size()); ++i) {
            Slot* s = d.slots[size_t(i)].get();
            if (i == d.latest || s->leases > 0 || s->writing) continue;
            if (!slot || s->frame.sequence < slot->frame.sequence) {
                slot = s;
                index = i;
            }
        }
        if (!slot) {
            d.dropped.fetch_add(1);
            return false;
        }
        slot->writing = true;
    }

    uint32_t w = std::min(uint32_t(std::max(content.width, 0)), sw);
    uint32_t h = std::min(uint32_t(std::max(content.height, 0)), sh);
    if (w == 0 || h == 0) {
        w = sw;
        h = sh;
    }
    uint64_t value = 0;
    bool ok = false;
    @autoreleasepool {
        auto& dev = d.device->impl();
        id<MTLTexture> src = texture_for(dev.device, src_surface, MTLTextureUsageShaderRead);
        id<MTLCommandBuffer> cmd = [dev.queue commandBuffer];
        if (src && cmd) {
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit copyFromTexture:src
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(w, h, 1)
                        toTexture:slot->texture
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            value = ++d.event_value;
            [cmd encodeSignalEvent:d.event value:value];
            [cmd commit];
            ok = true;
        }
    }

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        slot->writing = false;
        if (!ok) {
            d.dropped.fetch_add(1);
            return false;
        }
        slot->has_frame = true;
        Frame& f = slot->frame;
        f.sequence = ++d.sequence;
        f.image_id = slot->desc.id;
        f.content = Size{int32_t(w), int32_t(h)};
        f.damage.clear();
        f.wait_value = value;
        f.timestamp_ns = timestamp_ns;
        d.latest = index;
        cb = d.callback;
    }
    if (cb) cb();
    return true;
}

void SurfaceRing::close() { impl_->is_closed.store(true); }

std::optional<Frame> SurfaceRing::acquire() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->latest < 0) return std::nullopt;
    Slot& s = *impl_->slots[size_t(impl_->latest)];
    ++s.leases;
    return s.frame;
}

void SurfaceRing::release(const Frame& frame) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& s : impl_->slots) {
        if (s->desc.id == frame.image_id && s->leases > 0) {
            --s->leases;
            return;
        }
    }
    auto& r = impl_->retiring;
    for (auto it = r.begin(); it != r.end(); ++it) {
        if ((*it)->desc.id == frame.image_id && (*it)->leases > 0) {
            if (--(*it)->leases == 0) r.erase(it);
            return;
        }
    }
}

std::vector<SharedImage> SurfaceRing::images() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<SharedImage> out;
    for (auto& s : impl_->slots) out.push_back(s->desc);
    return out;
}

std::optional<SharedImage> SurfaceRing::image(uint64_t id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& s : impl_->slots)
        if (s->desc.id == id) return s->desc;
    for (auto& s : impl_->retiring)
        if (s->desc.id == id) return s->desc;
    return std::nullopt;
}

uint64_t SurfaceRing::images_generation() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->generation;
}

SharedTimeline SurfaceRing::timeline() const {
    SharedTimeline t;
    t.type = SyncHandleType::MetalSharedEvent;
    t.handle.value = uint64_t(reinterpret_cast<uintptr_t>((__bridge void*)impl_->event));
    t.id = impl_->event_id;
    return t;
}

bool SurfaceRing::closed() const { return impl_->is_closed.load(); }

void SurfaceRing::set_frame_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

uint64_t SurfaceRing::frames_dropped() const { return impl_->dropped.load(); }

}  // namespace brocompositor::mac
