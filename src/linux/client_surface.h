// ClientSurfaceImpl: one wl_surface as a SurfaceSource.
//
// Image registry: every distinct wlr_buffer a client attached becomes a
// SharedImage (dmabuf planes or shm fd, dup()ed so the fds stay valid while
// listed or leased) and is retired when wlroots destroys the buffer. The
// server holds a wlr_buffer lock on the current image and on every leased
// image, so the client gets wl_buffer.release only once the host is done.
// Buffers that are neither dmabuf nor shm (single-pixel buffers, ...) are
// copied into a memfd image.
#pragma once

#include "brocompositor/linux/server.h"
#include "linux/desktop_records.h"
#include "linux/dispatcher.h"
#include "linux/wlr.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace brocompositor::wl {

struct Server;

class ClientSurfaceImpl final : public ClientSurface, public std::enable_shared_from_this<ClientSurfaceImpl> {
public:
    // `gate`: while the session is locked, acquire() only serves lock surfaces.
    ClientSurfaceImpl(Server* server, std::shared_ptr<Dispatcher> dispatcher, std::shared_ptr<LockGate> gate,
                      SurfaceId id, wlr_surface* surface);
    ~ClientSurfaceImpl() override;

    // ---- SurfaceSource / ClientSurface (any thread) ----
    std::optional<Frame> acquire() override;
    void release(const Frame& frame) override;
    std::vector<SharedImage> images() const override;
    std::optional<SharedImage> image(uint64_t id) const override;
    uint64_t images_generation() const override;
    SharedTimeline timeline() const override;
    void presented(int64_t timestamp_ns) override;
    bool closed() const override;
    SurfaceId id() const override { return id_; }
    SurfaceState state() const override;
    void presented_on(MonitorId output, int64_t timestamp_ns) override;

    // ---- server thread ----
    // Handles a commit; returns true when a new buffer was attached.
    bool on_commit();
    void on_destroy();
    // Server teardown: drops every buffer lock and buffer listener.
    void shutdown();
    uint64_t sequence() const;

private:
    struct Img {
        SharedImage desc;
        std::vector<int> fds;
        wlr_buffer* buffer = nullptr;  // server thread only; null for copies
        bool locked = false;
        uint32_t leases = 0;
        bool copy = false;
        size_t copy_size = 0;
        std::unique_ptr<Listener> buffer_destroy;
    };
    struct FrameRec {
        Frame frame;
        uint32_t leases = 0;
        int sync_fd = -1;
    };

    Img* register_buffer(wlr_buffer* buffer, std::vector<wlr_buffer*>* to_unlock);
    Img* copy_buffer(wlr_buffer* buffer);
    void retire_image_locked(uint64_t image_id);
    void unlock_if_idle(uint64_t image_id);
    void drop_superseded_frames_locked();
    bool is_current_locked(uint64_t image_id) const;

    Server* server_;
    std::shared_ptr<Dispatcher> dispatcher_;
    std::shared_ptr<LockGate> gate_;
    const SurfaceId id_;
    wlr_surface* surface_;  // server thread only

    mutable std::mutex m_;
    std::map<uint64_t, std::unique_ptr<Img>> images_;
    std::map<wlr_buffer*, uint64_t> by_buffer_;  // server thread only
    std::deque<FrameRec> frames_;  // back() is current (when has_current_)
    bool has_current_ = false;
    uint64_t sequence_ = 0;
    uint64_t generation_ = 1;
    std::vector<Rect> pending_damage_;
    bool damage_full_ = true;
    bool closed_ = false;
    SurfaceState state_;
};

}  // namespace brocompositor::wl
