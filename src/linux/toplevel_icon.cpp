// xdg-toplevel-icon-v1: a client's own icon for its toplevel (an image, or
// a theme icon name), which wlroots 0.18 has no helper for. The pixels are
// copied out of the client's wl_shm buffers when they are added, converted
// to straight-alpha RGBA, and kept per toplevel as an immutable IconSet; the
// window's snapshot carries the name and a serial that changes with the
// icon (change::Icon), and ServerBackend::window_icon hands out the image.
#include "linux/server_impl.h"

#if BC_HAVE_TOPLEVEL_ICON
#include "xdg-toplevel-icon-v1-protocol.h"
#endif

#include <drm_fourcc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace brocompositor::wl {

struct ServerBackend::Impl : Server {};

#if BC_HAVE_TOPLEVEL_ICON
namespace {

// The sizes asked for (icon_size): a title bar's, a taskbar's, a switcher's.
constexpr int32_t kWantedSizes[] = {32, 48, 64, 128};

struct IconObj {
    Server* srv = nullptr;
    std::shared_ptr<IconSet> set = std::make_shared<IconSet>();
    bool immutable = false;
};

IconObj* icon_of(wl_resource* r) { return static_cast<IconObj*>(wl_resource_get_user_data(r)); }

void icon_destroy_resource(wl_resource* r) { delete icon_of(r); }

// wl_shm ARGB8888 / XRGB8888 (premultiplied, little-endian B G R A) and
// ABGR8888 / XBGR8888 (R G B A) into straight-alpha RGBA.
bool copy_pixels(wlr_buffer* b, std::vector<uint8_t>& out) {
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(b, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) return false;
    const bool bgr = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
    const bool rgb = format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888;
    const bool opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;
    if (!bgr && !rgb) {
        wlr_buffer_end_data_ptr_access(b);
        return false;
    }
    const int w = b->width, h = b->height;
    out.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        const auto* s = static_cast<const uint8_t*>(data) + size_t(y) * stride;
        uint8_t* d = out.data() + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x, s += 4, d += 4) {
            const uint8_t a = opaque ? 255 : s[3];
            uint8_t r = bgr ? s[2] : s[0], g = s[1], bl = bgr ? s[0] : s[2];
            if (a != 0 && a != 255) {
                r = uint8_t(std::min(255, (r * 255 + a / 2) / a));
                g = uint8_t(std::min(255, (g * 255 + a / 2) / a));
                bl = uint8_t(std::min(255, (bl * 255 + a / 2) / a));
            }
            d[0] = r;
            d[1] = g;
            d[2] = bl;
            d[3] = a;
        }
    }
    wlr_buffer_end_data_ptr_access(b);
    return true;
}

const struct xdg_toplevel_icon_v1_interface kIconImpl = {
    // destroy
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    // set_name
    [](wl_client*, wl_resource* r, const char* name) {
        IconObj* o = icon_of(r);
        if (o->immutable) {
            wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE, "the icon was set on a toplevel");
            return;
        }
        o->set->name = name ? name : "";
    },
    // add_buffer
    [](wl_client*, wl_resource* r, wl_resource* buffer, int32_t scale) {
        IconObj* o = icon_of(r);
        if (o->immutable) {
            wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE, "the icon was set on a toplevel");
            return;
        }
        wlr_buffer* b = wlr_buffer_try_from_resource(buffer);
        if (!b || b->width != b->height || b->width <= 0) {
            if (b) wlr_buffer_unlock(b);
            wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER, "the buffer is not a square wl_shm buffer");
            return;
        }
        WindowIcon img;
        img.size = b->width;
        const bool ok = copy_pixels(b, img.rgba);
        wlr_buffer_unlock(b);
        if (!ok) {
            wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER, "the buffer is not a readable wl_shm buffer");
            return;
        }
        (void)scale;  // kept by pixel size: what a host draws is pixels
        auto& imgs = o->set->images;
        imgs.erase(std::remove_if(imgs.begin(), imgs.end(), [&](const WindowIcon& i) { return i.size == img.size; }),
                   imgs.end());
        imgs.push_back(std::move(img));
    },
};

const struct xdg_toplevel_icon_manager_v1_interface kManagerImpl = {
    // destroy
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    // create_icon
    [](wl_client* client, wl_resource* m, uint32_t id) {
        wl_resource* r = wl_resource_create(client, &xdg_toplevel_icon_v1_interface, wl_resource_get_version(m), id);
        if (!r) {
            wl_client_post_no_memory(client);
            return;
        }
        auto* o = new IconObj;
        o->srv = static_cast<Server*>(wl_resource_get_user_data(m));
        wl_resource_set_implementation(r, &kIconImpl, o, icon_destroy_resource);
    },
    // set_icon
    [](wl_client*, wl_resource* m, wl_resource* toplevel, wl_resource* icon) {
        auto* srv = static_cast<Server*>(wl_resource_get_user_data(m));
        wlr_xdg_toplevel* xdg = wlr_xdg_toplevel_from_resource(toplevel);
        std::shared_ptr<const IconSet> set;
        if (icon) {
            IconObj* o = icon_of(icon);
            o->immutable = true;
            // An icon with neither pixels nor a name resets, like null.
            if (!o->set->images.empty() || !o->set->name.empty()) set = o->set;
        }
        if (xdg) srv->set_toplevel_icon(xdg, std::move(set));
    },
};

void bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &xdg_toplevel_icon_manager_v1_interface,
                                        int(std::min<uint32_t>(version, 1)), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kManagerImpl, data, nullptr);
    for (int32_t s : kWantedSizes) xdg_toplevel_icon_manager_v1_send_icon_size(r, s);
    xdg_toplevel_icon_manager_v1_send_done(r);
}

}  // namespace

void Server::init_toplevel_icon() {
    toplevel_icon_global = wl_global_create(display, &xdg_toplevel_icon_manager_v1_interface, 1, this, bind);
}
#else
void Server::init_toplevel_icon() {}
#endif

void Server::set_toplevel_icon(wlr_xdg_toplevel* xdg, std::shared_ptr<const IconSet> icon) {
    auto it = toplevels.find(xdg);
    if (it == toplevels.end()) return;
    ToplevelRec& t = *it->second;
    t.pending_icon = std::move(icon);
    t.icon_pending = true;
}

void Server::apply_toplevel_icon(ToplevelRec& t) {
    t.icon_pending = false;
    if (t.pending_icon == t.icon) {
        t.pending_icon.reset();
        return;
    }
    t.icon = std::move(t.pending_icon);
    t.pending_icon.reset();
    static uint64_t serial = 0;
    t.icon_serial = t.icon ? ++serial : 0;
    publish_window(t, change::Icon);
}

std::optional<WindowIcon> ServerBackend::window_icon(WindowId id, int32_t size) const {
    std::shared_ptr<const IconSet> set;
    {
        std::lock_guard<std::mutex> lock(impl_->mirror.m);
        auto it = impl_->mirror.windows.find(id);
        if (it == impl_->mirror.windows.end() || !it->second.icon) return std::nullopt;
        set = it->second.icon;
    }
    WindowIcon out;
    out.name = set->name;
    const WindowIcon* best = nullptr;
    for (const WindowIcon& i : set->images) {
        if (!best) {
            best = &i;
            continue;
        }
        if (size <= 0) {
            if (i.size > best->size) best = &i;
            continue;
        }
        // The smallest at least `size`, else the largest.
        const bool iFits = i.size >= size, bFits = best->size >= size;
        if (iFits != bFits ? iFits : (iFits ? i.size < best->size : i.size > best->size)) best = &i;
    }
    if (best) {
        out.size = best->size;
        out.rgba = best->rgba;
    }
    return out;
}

}  // namespace brocompositor::wl
