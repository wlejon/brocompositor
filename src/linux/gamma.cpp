// wlr-gamma-control-unstable-v1 (wlsunset, gammastep, wl-gammarelay). With
// a hardware LUT (KMS) the ramps go out with the next output commit; without
// one (nested, headless, or a host that composites with its own colour
// pipeline) the host applies them: GammaChanged / ServerBackend::gamma()
// carry the ramps at ServerConfig::host_gamma_size entries per channel.
// Destroying the control (or the client dying) restores the identity ramps.
#include "linux/server_impl.h"

#include "wlr-gamma-control-unstable-v1-protocol.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

namespace brocompositor::wl {

namespace {

struct GammaCtl {
    Server* srv = nullptr;
    MonitorId output = kNoMonitor;  // kNoMonitor: inert (failed / output gone)
};

GammaCtl* ctl_of(wl_resource* r) { return static_cast<GammaCtl*>(wl_resource_get_user_data(r)); }

void make_inert(wl_resource* r) {
    if (GammaCtl* c = ctl_of(r)) c->output = kNoMonitor;
}

// Reads exactly `n` bytes (the client may hand a pipe).
bool read_all(int fd, void* dst, size_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += size_t(r);
    }
    return true;
}

void publish(Server* s, OutputRec& out) {
    GammaChanged ev{out.id, out.gamma.size, out.gamma.ramps, out.gamma.hardware};
    {
        std::lock_guard<std::mutex> lock(s->mirror.m);
        if (ev.ramps.empty())
            s->mirror.gamma.erase(out.id);
        else
            s->mirror.gamma[out.id] = ev;
    }
    s->server_events.push(std::move(ev));
    if (out.gamma.hardware) out.gamma.pending = true;
    wlr_output_schedule_frame(out.output);
}

void restore(Server* s, OutputRec& out) {
    out.gamma.control = nullptr;
    if (out.gamma.ramps.empty()) return;
    out.gamma.ramps.clear();
    publish(s, out);
}

const struct zwlr_gamma_control_v1_interface kControlImpl = {
    [](wl_client*, wl_resource* r, int32_t fd) {
        GammaCtl* c = ctl_of(r);
        OutputRec* out = c && c->output != kNoMonitor ? c->srv->output_rec(c->output) : nullptr;
        if (!out || out->gamma.control != r) {
            ::close(fd);
            return;
        }
        std::vector<uint16_t> ramps(size_t(out->gamma.size) * 3);
        // A blocking read: wlroots does the same; the fd is a memfd / file in practice.
        int flags = fcntl(fd, F_GETFL);
        if (flags != -1) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        bool ok = read_all(fd, ramps.data(), ramps.size() * sizeof(uint16_t));
        ::close(fd);
        if (!ok) {
            zwlr_gamma_control_v1_send_failed(r);
            make_inert(r);
            restore(c->srv, *out);
            return;
        }
        out->gamma.ramps = std::move(ramps);
        publish(c->srv, *out);
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

void control_destroy(wl_resource* r) {
    GammaCtl* c = ctl_of(r);
    if (c && c->output != kNoMonitor)
        if (OutputRec* out = c->srv->output_rec(c->output); out && out->gamma.control == r) restore(c->srv, *out);
    delete c;
}

const struct zwlr_gamma_control_manager_v1_interface kManagerImpl = {
    [](wl_client* client, wl_resource* m, uint32_t id, wl_resource* output) {
        auto* s = static_cast<Server*>(wl_resource_get_user_data(m));
        wl_resource* r =
            wl_resource_create(client, &zwlr_gamma_control_v1_interface, wl_resource_get_version(m), id);
        if (!r) {
            wl_client_post_no_memory(client);
            return;
        }
        auto* c = new GammaCtl{s, kNoMonitor};
        wl_resource_set_implementation(r, &kControlImpl, c, control_destroy);
        wlr_output* wo = wlr_output_from_resource(output);
        OutputRec* out = wo ? s->output_rec(wo) : nullptr;
        if (!out) {
            zwlr_gamma_control_v1_send_failed(r);
            return;
        }
        size_t hw = wlr_output_get_gamma_size(wo);
        uint32_t size = hw ? uint32_t(hw) : s->config.host_gamma_size;
        // One control per output; none at all without a LUT of any kind.
        if (size == 0 || out->gamma.control) {
            zwlr_gamma_control_v1_send_failed(r);
            return;
        }
        c->output = out->id;
        out->gamma.control = r;
        out->gamma.size = size;
        out->gamma.hardware = hw != 0;
        zwlr_gamma_control_v1_send_gamma_size(r, size);
    },
    [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

void bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* r =
        wl_resource_create(client, &zwlr_gamma_control_manager_v1_interface, int(std::min<uint32_t>(version, 1)), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kManagerImpl, data, nullptr);
}

}  // namespace

void Server::init_gamma() {
    gamma_global = wl_global_create(display, &zwlr_gamma_control_manager_v1_interface, 1, this, bind);
}

void Server::apply_gamma(OutputRec& out, wlr_output_state* state) {
    if (!out.gamma.hardware || !out.gamma.pending) return;
    out.gamma.pending = false;
    const std::vector<uint16_t>& g = out.gamma.ramps;
    if (g.empty()) {
        wlr_output_state_set_gamma_lut(state, 0, nullptr, nullptr, nullptr);
    } else {
        size_t n = out.gamma.size;
        wlr_output_state_set_gamma_lut(state, n, g.data(), g.data() + n, g.data() + 2 * n);
    }
}

void Server::gamma_output_gone(OutputRec& out) {
    if (wl_resource* r = out.gamma.control) {
        zwlr_gamma_control_v1_send_failed(r);
        make_inert(r);
    }
    out.gamma = GammaState{};
    std::lock_guard<std::mutex> lock(mirror.m);
    mirror.gamma.erase(out.id);
}

}  // namespace brocompositor::wl
