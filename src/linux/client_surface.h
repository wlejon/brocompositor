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
#include <set>
#include <vector>

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
    void presented_with(const PresentationTime& t) override;
    void presented_frame(const Frame& frame, const PresentationTime& t) override;

    // ---- server thread ----
    // The newest commit's presentation feedback, taken (the output path
    // sends it when its output commit is presented); null when none.
    wlr_presentation_feedback* take_current_feedback();
    // Handles a commit; returns true when a new buffer was attached.
    bool on_commit();
    bool apply_commit();  // on_commit's buffer and frame half
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
    // Feedback of commits that can no longer be presented (superseded, not
    // leased, not being presented) moves to `drop` (destroyed: discarded).
    void prune_feedback_locked(std::vector<wlr_presentation_feedback*>* drop);
    // Sends presented (or discarded without an output) for `seq`'s feedback,
    // discarding older unpresentable ones, then the frame callbacks.
    void send_presented(uint64_t seq, const PresentationTime& t);

    Server* server_;
    std::shared_ptr<Dispatcher> dispatcher_;
    std::shared_ptr<LockGate> gate_;
    const SurfaceId id_;
    wlr_surface* surface_;  // server thread only

    mutable std::mutex m_;
    std::map<uint64_t, std::unique_ptr<Img>> images_;
    std::map<wlr_buffer*, uint64_t> by_buffer_;  // server thread only
    std::deque<FrameRec> frames_;  // back() is current (when has_current_)
    // Presentation feedback taken at each commit, by frame sequence (server
    // thread only; frame 0 is a commit before any buffer). wlroots keeps one
    // per surface and hands out whichever is newest when asked, so it is
    // taken at commit and kept with the frame it belongs to.
    std::map<uint64_t, wlr_presentation_feedback*> feedback_;
    std::set<uint64_t> presenting_;  // frames presented_frame() was called for, not yet sent (m_)
    bool has_current_ = false;
    uint64_t sequence_ = 0;
    uint64_t generation_ = 1;
    std::vector<Rect> pending_damage_;
    bool damage_full_ = true;
    bool closed_ = false;
    SurfaceState state_;
};

}  // namespace brocompositor::wl
