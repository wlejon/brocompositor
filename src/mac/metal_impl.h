// Private Objective-C++ state of mac::MetalDevice (included by .mm files only).
#pragma once

#import <Metal/Metal.h>

#include "brocompositor/mac/capture.h"

namespace brocompositor::mac {

struct MetalDevice::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    AdapterId adapter;
};

// MoltenVK reports MTLDevice.registryID big-endian in deviceLUID; the
// producer fills both so AdapterId compares equal with vk::adapter_of().
inline AdapterId adapter_of_registry(uint64_t registry_id) {
    AdapterId a;
    a.metal_registry_id = registry_id;
    for (int i = 0; i < 8; ++i) a.luid[size_t(i)] = uint8_t(registry_id >> (56 - 8 * i));
    return a;
}

}  // namespace brocompositor::mac
