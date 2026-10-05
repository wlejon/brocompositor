#include "linux/client_surface.h"

#include "linux/drm_util.h"
#include "linux/server_impl.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace brocompositor::wl {

namespace {

std::atomic<uint64_t> g_image_id{1};

SharedImage describe_dmabuf(const wlr_dmabuf_attributes& a, std::vector<int>* fds) {
    SharedImage d;
    d.type = ImageHandleType::DmaBuf;
    d.width = uint32_t(a.width);
    d.height = uint32_t(a.height);
    d.drm_format = a.format;
    d.format = pixel_format_of(a.format);
    d.drm_modifier = a.modifier;
    for (int i = 0; i < a.n_planes; ++i) {
        int fd = fcntl(a.fd[i], F_DUPFD_CLOEXEC, 3);
        fds->push_back(fd);
        d.planes.push_back(SharedPlane{from_fd(fd), a.offset[i], a.stride[i]});
    }
    return d;
}

}  // namespace

uint64_t next_image_id() { return g_image_id.fetch_add(1); }

ClientSurfaceImpl::ClientSurfaceImpl(Server* server, std::shared_ptr<Dispatcher> dispatcher,
                                     std::shared_ptr<LockGate> gate, SurfaceId id, wlr_surface* surface)
    : server_(server), dispatcher_(std::move(dispatcher)), gate_(std::move(gate)), id_(id), surface_(surface) {}

ClientSurfaceImpl::~ClientSurfaceImpl() {
    for (auto& [id, img] : images_)
        for (int fd : img->fds)
            if (fd >= 0) ::close(fd);
    for (auto& f : frames_)
        if (f.sync_fd >= 0) ::close(f.sync_fd);
}

// ---------------------------------------------------------------- host side

std::optional<Frame> ClientSurfaceImpl::acquire() {
    // The session lock withholds every surface but the lock surfaces, even
    // from hosts that kept a ClientSurface from before the lock.
    if (gate_ && !gate_->allows(id_)) return std::nullopt;
    std::lock_guard<std::mutex> lock(m_);
    if (!has_current_ || frames_.empty()) return std::nullopt;
    FrameRec& cur = frames_.back();
    auto it = images_.find(cur.frame.image_id);
    if (it == images_.end()) return std::nullopt;
    ++cur.leases;
    ++it->second->leases;
    Frame f = cur.frame;
    if (damage_full_) f.damage.clear();
    else f.damage = pending_damage_;
    pending_damage_.clear();
    damage_full_ = false;
    return f;
}

void ClientSurfaceImpl::release(const Frame& frame) {
    bool unlock = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        for (auto& f : frames_)
            if (f.frame.sequence == frame.sequence && f.leases) {
                --f.leases;
                break;
            }
        auto it = images_.find(frame.image_id);
        if (it != images_.end() && it->second->leases) {
            --it->second->leases;
            unlock = it->second->leases == 0 && !is_current_locked(frame.image_id) && it->second->locked;
        }
        drop_superseded_frames_locked();
    }
    if (unlock) {
        uint64_t image_id = frame.image_id;
        // Keep the surface alive until the job ran.
        auto self = shared_from_this();
        dispatcher_->post([self, image_id] { self->unlock_if_idle(image_id); });
    }
}

std::vector<SharedImage> ClientSurfaceImpl::images() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<SharedImage> out;
    for (auto& [id, img] : images_) out.push_back(img->desc);
    return out;
}

std::optional<SharedImage> ClientSurfaceImpl::image(uint64_t id) const {
    std::lock_guard<std::mutex> lock(m_);
    auto it = images_.find(id);
    if (it == images_.end()) return std::nullopt;
    return it->second->desc;
}

uint64_t ClientSurfaceImpl::images_generation() const {
    std::lock_guard<std::mutex> lock(m_);
    return generation_;
}

SharedTimeline ClientSurfaceImpl::timeline() const {
    // Per-frame sync files (Frame::sync_fd); there is no shared timeline.
    return SharedTimeline{SyncHandleType::SyncFileFd, kNoFd, 0};
}

void ClientSurfaceImpl::presented(int64_t timestamp_ns) { presented_on(kNoMonitor, timestamp_ns); }

void ClientSurfaceImpl::presented_on(MonitorId output, int64_t timestamp_ns) {
    auto self = shared_from_this();
    dispatcher_->post([self, output, timestamp_ns] {
        wlr_surface* s = self->surface_;
        if (!s) return;
        Server* srv = self->server_;
        if (output != kNoMonitor) {
            if (OutputRec* o = srv->output_rec(output)) {
                if (auto* fb = wlr_presentation_surface_sampled(s)) {
                    wlr_presentation_event ev{};
                    ev.output = o->output;
                    ev.tv_sec = uint64_t(timestamp_ns / 1000000000);
                    ev.tv_nsec = uint32_t(timestamp_ns % 1000000000);
                    ev.refresh = o->output->refresh ? uint32_t(1000000000000ll / o->output->refresh) : 0;
                    wlr_presentation_feedback_send_presented(fb, &ev);
                    wlr_presentation_feedback_destroy(fb);
                }
            }
        }
        srv->send_frame_done(s, timestamp_ns);
    });
}

bool ClientSurfaceImpl::closed() const {
    std::lock_guard<std::mutex> lock(m_);
    return closed_;
}

SurfaceState ClientSurfaceImpl::state() const {
    std::lock_guard<std::mutex> lock(m_);
    return state_;
}

uint64_t ClientSurfaceImpl::sequence() const {
    std::lock_guard<std::mutex> lock(m_);
    return sequence_;
}

// ---------------------------------------------------------------- server side

bool ClientSurfaceImpl::is_current_locked(uint64_t image_id) const {
    return has_current_ && !frames_.empty() && frames_.back().frame.image_id == image_id;
}

void ClientSurfaceImpl::drop_superseded_frames_locked() {
    // Frames other than the current one live only while leased.
    size_t keep_from = has_current_ ? frames_.size() - 1 : frames_.size();
    for (size_t i = 0; i < keep_from && i < frames_.size();) {
        if (frames_[i].leases == 0) {
            if (frames_[i].sync_fd >= 0) ::close(frames_[i].sync_fd);
            frames_.erase(frames_.begin() + long(i));
            --keep_from;
        } else {
            ++i;
        }
    }
}

void ClientSurfaceImpl::unlock_if_idle(uint64_t image_id) {
    wlr_buffer* b = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = images_.find(image_id);
        if (it == images_.end()) return;
        Img& img = *it->second;
        if (img.leases || is_current_locked(image_id) || !img.locked) return;
        img.locked = false;
        b = img.buffer;
    }
    if (b) wlr_buffer_unlock(b);  // may release / destroy the buffer
}

void ClientSurfaceImpl::retire_image_locked(uint64_t image_id) {
    auto it = images_.find(image_id);
    if (it == images_.end()) return;
    for (int fd : it->second->fds)
        if (fd >= 0) ::close(fd);
    if (it->second->buffer) by_buffer_.erase(it->second->buffer);
    images_.erase(it);
    ++generation_;
}

ClientSurfaceImpl::Img* ClientSurfaceImpl::register_buffer(wlr_buffer* buffer, std::vector<wlr_buffer*>* to_unlock) {
    (void)to_unlock;
    auto known = by_buffer_.find(buffer);
    if (known != by_buffer_.end()) {
        auto it = images_.find(known->second);
        if (it != images_.end()) return it->second.get();
    }
    auto img = std::make_unique<Img>();
    wlr_dmabuf_attributes dma{};
    wlr_shm_attributes shm{};
    if (wlr_buffer_get_dmabuf(buffer, &dma)) {
        img->desc = describe_dmabuf(dma, &img->fds);
    } else if (wlr_buffer_get_shm(buffer, &shm)) {
        int fd = fcntl(shm.fd, F_DUPFD_CLOEXEC, 3);
        img->fds.push_back(fd);
        img->desc.type = ImageHandleType::ShmFd;
        img->desc.width = uint32_t(shm.width);
        img->desc.height = uint32_t(shm.height);
        img->desc.drm_format = shm_to_drm(shm.format);
        img->desc.format = pixel_format_of(img->desc.drm_format);
        img->desc.planes.push_back(SharedPlane{from_fd(fd), uint32_t(shm.offset), uint32_t(shm.stride)});
    } else {
        return nullptr;
    }
    img->desc.id = next_image_id();
    img->desc.adapter = server_->adapter;
    img->buffer = buffer;
    uint64_t image_id = img->desc.id;
    img->buffer_destroy = std::make_unique<Listener>();
    img->buffer_destroy->connect(&buffer->events.destroy, [this, image_id, buffer](void*) {
        std::lock_guard<std::mutex> lock(m_);
        by_buffer_.erase(buffer);
        auto it = images_.find(image_id);
        if (it == images_.end()) return;
        it->second->buffer_destroy->disconnect();
        it->second->buffer = nullptr;
        it->second->locked = false;
        // Only unlocked buffers are destroyed, and a leased image is locked.
        retire_image_locked(image_id);
    });
    Img* raw = img.get();
    by_buffer_[buffer] = image_id;
    images_[image_id] = std::move(img);
    ++generation_;
    return raw;
}

ClientSurfaceImpl::Img* ClientSurfaceImpl::copy_buffer(wlr_buffer* buffer) {
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride))
        return nullptr;
    size_t row = size_t(buffer->width) * 4;
    size_t size = row * size_t(buffer->height);
    Img* target = nullptr;
    // Reuse the current copy image when nobody leases it.
    if (has_current_ && !frames_.empty()) {
        auto it = images_.find(frames_.back().frame.image_id);
        if (it != images_.end() && it->second->copy && it->second->leases == 0 && it->second->copy_size == size &&
            it->second->desc.drm_format == format)
            target = it->second.get();
    }
    if (!target) {
        int fd = create_memfd("brocompositor-copy", size);
        if (fd < 0) {
            wlr_buffer_end_data_ptr_access(buffer);
            return nullptr;
        }
        auto img = std::make_unique<Img>();
        img->copy = true;
        img->copy_size = size;
        img->fds.push_back(fd);
        img->desc.id = next_image_id();
        img->desc.type = ImageHandleType::ShmFd;
        img->desc.width = uint32_t(buffer->width);
        img->desc.height = uint32_t(buffer->height);
        img->desc.drm_format = format;
        img->desc.format = pixel_format_of(format);
        img->desc.planes.push_back(SharedPlane{from_fd(fd), 0, uint32_t(row)});
        img->desc.adapter = server_->adapter;
        target = img.get();
        images_[img->desc.id] = std::move(img);
        ++generation_;
    }
    void* dst = mmap(nullptr, size, PROT_WRITE, MAP_SHARED, target->fds[0], 0);
    if (dst != MAP_FAILED) {
        for (int y = 0; y < buffer->height; ++y)
            std::memcpy(static_cast<uint8_t*>(dst) + size_t(y) * row,
                        static_cast<const uint8_t*>(data) + size_t(y) * stride, std::min(row, stride));
        munmap(dst, size);
    }
    wlr_buffer_end_data_ptr_access(buffer);
    return target;
}

bool ClientSurfaceImpl::on_commit() {
    wlr_surface* s = surface_;
    bool new_buffer = (s->current.committed & WLR_SURFACE_STATE_BUFFER) != 0;
    wlr_buffer* buffer = new_buffer ? s->current.buffer : nullptr;

    SurfaceState st;
    st.size = Size{s->current.width, s->current.height};
    st.buffer_size = Size{s->current.buffer_width, s->current.buffer_height};
    st.buffer_scale = s->current.scale;
    st.transform = uint32_t(s->current.transform);
    st.has_source_crop = s->current.viewport.has_src;
    if (st.has_source_crop) {
        st.src_x = s->current.viewport.src.x;
        st.src_y = s->current.viewport.src.y;
        st.src_width = s->current.viewport.src.width;
        st.src_height = s->current.viewport.src.height;
    }
    st.opaque = s->opaque;
    st.mapped = s->mapped;

    int sync_fd = -1;
    if (buffer) {
        wlr_dmabuf_attributes dma{};
        if (wlr_buffer_get_dmabuf(buffer, &dma) && dma.n_planes > 0) sync_fd = export_sync_file(dma.fd[0]);
    }

    std::vector<wlr_buffer*> unlock;
    {
        std::lock_guard<std::mutex> lock(m_);
        state_ = st;
        if (!new_buffer) {
            // Damage without a new buffer is meaningless; keep accumulating.
            return false;
        }
        uint64_t old_image = has_current_ && !frames_.empty() ? frames_.back().frame.image_id : 0;
        if (!buffer) {
            has_current_ = false;
        } else {
            Img* img = register_buffer(buffer, &unlock);
            if (!img) img = copy_buffer(buffer);
            if (!img) {
                if (sync_fd >= 0) ::close(sync_fd);
                return false;
            }
            if (img->buffer && !img->locked) {
                wlr_buffer_lock(buffer);
                img->locked = true;
            }
            FrameRec rec;
            rec.frame.sequence = ++sequence_;
            rec.frame.image_id = img->desc.id;
            rec.frame.content = Size{buffer->width, buffer->height};
            int n = 0;
            const pixman_box32_t* boxes = pixman_region32_rectangles(&s->buffer_damage, &n);
            bool full = n == 0;
            for (int i = 0; i < n; ++i)
                rec.frame.damage.push_back(Rect{boxes[i].x1, boxes[i].y1, boxes[i].x2 - boxes[i].x1,
                                                boxes[i].y2 - boxes[i].y1});
            if (!full) {
                Rect all{0, 0, buffer->width, buffer->height};
                full = rec.frame.damage.size() == 1 && rec.frame.damage[0].contains(all);
            }
            if (full) {
                damage_full_ = true;
                pending_damage_.clear();
            } else if (!damage_full_) {
                pending_damage_.insert(pending_damage_.end(), rec.frame.damage.begin(), rec.frame.damage.end());
            }
            rec.sync_fd = sync_fd;
            rec.frame.sync_fd = from_fd(sync_fd);
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            rec.frame.timestamp_ns = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
            frames_.push_back(std::move(rec));
            has_current_ = true;
        }
        drop_superseded_frames_locked();
        if (old_image && !is_current_locked(old_image)) {
            auto it = images_.find(old_image);
            if (it != images_.end() && it->second->leases == 0) {
                if (it->second->locked) {
                    it->second->locked = false;
                    unlock.push_back(it->second->buffer);
                }
            }
        }
    }
    for (wlr_buffer* b : unlock)
        if (b) wlr_buffer_unlock(b);
    return true;
}

void ClientSurfaceImpl::on_destroy() {
    std::vector<wlr_buffer*> unlock;
    {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        has_current_ = false;
        state_.mapped = false;
        // From here on the server forgets this surface; the host may keep the
        // object alive. Buffer listeners are detached now (on the server
        // thread); leased images stay locked until their release() runs
        // unlock_if_idle() on the server thread.
        for (auto& [id, img] : images_) {
            if (img->buffer_destroy) img->buffer_destroy->disconnect();
            if (img->locked && img->leases == 0) {
                img->locked = false;
                unlock.push_back(img->buffer);
            }
            if (!img->locked) img->buffer = nullptr;
        }
        by_buffer_.clear();
        drop_superseded_frames_locked();
        surface_ = nullptr;
    }
    for (wlr_buffer* b : unlock)
        if (b) wlr_buffer_unlock(b);
}

void ClientSurfaceImpl::shutdown() {
    std::vector<wlr_buffer*> unlock;
    {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        has_current_ = false;
        for (auto& [id, img] : images_) {
            if (img->buffer_destroy) img->buffer_destroy->disconnect();
            if (img->locked) unlock.push_back(img->buffer);
            img->locked = false;
            img->buffer = nullptr;
        }
        by_buffer_.clear();
    }
    for (wlr_buffer* b : unlock)
        if (b) wlr_buffer_unlock(b);
}

}  // namespace brocompositor::wl
