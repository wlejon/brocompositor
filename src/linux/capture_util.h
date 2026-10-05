// Helpers shared by the capture protocols (wlr-screencopy,
// ext-image-copy-capture): what a capture buffer of an output must look
// like, per-client damage accumulation, and timestamps.
#pragma once

#include "linux/server_impl.h"

#include <memory>
#include <vector>

namespace brocompositor::wl {

// The buffer a full-output capture needs: the output image's format and
// pixel size (XRGB8888 at the mode size before the host made images).
struct OutputCaptureInfo {
    uint32_t drm_format = 0;
    int width = 0, height = 0;
};
OutputCaptureInfo output_capture_info(OutputRec& out);

// The output image the host last presented (nullptr before the first one).
OutputImageSlot* front_slot(Server& s, OutputRec& out);

// wl_shm format code of a DRM fourcc (ARGB / XRGB are 0 / 1 in wl_shm).
uint32_t drm_to_shm(uint32_t drm_format);

// Whether a client buffer can take a capture of `w` x `h` pixels in
// `drm_format` (shm with that format, or a dmabuf of it).
bool capture_buffer_ok(wlr_buffer* b, uint32_t drm_format, int w, int h);

// PresentRequest::damage that says "nothing changed" (only empty rects);
// an empty vector means everything.
inline bool damage_is_none(const std::vector<Rect>& damage) {
    if (damage.empty()) return false;
    for (const Rect& r : damage)
        if (!r.empty()) return false;
    return true;
}

// Damage a capture client has not seen yet, in output pixels.
struct DamageAccum {
    bool full = true;  // everything (first capture, or too many rects)
    std::vector<Rect> rects;
    void add(const std::vector<Rect>& damage);
    bool empty() const { return !full && rects.empty(); }
    // The accumulated damage (full = one rect of w x h), and resets it.
    std::vector<Rect> take(int w, int h);
};

// Adds every present of `out` to `acc` for as long as `acc` lives.
void track_output_damage(OutputRec& out, std::weak_ptr<DamageAccum> acc);

inline void split_time(int64_t ns, uint32_t* sec_hi, uint32_t* sec_lo, uint32_t* nsec) {
    uint64_t sec = uint64_t(ns / 1000000000);
    *sec_hi = uint32_t(sec >> 32);
    *sec_lo = uint32_t(sec & 0xFFFFFFFFu);
    *nsec = uint32_t(ns % 1000000000);
}

int64_t monotonic_ns();

}  // namespace brocompositor::wl
