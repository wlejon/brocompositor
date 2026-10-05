// wlr-output-management: lets clients such as wlr-randr / kanshi list and
// reconfigure outputs. Applied configurations go through the same path as
// ServerBackend::configure_output().
#include "linux/server_impl.h"

namespace brocompositor::wl {

void Server::update_output_manager() {
    if (!output_manager) return;
    wlr_output_configuration_v1* cfg = wlr_output_configuration_v1_create();
    for (auto& [id, out] : outputs) {
        wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(cfg, out->output);
        wlr_box b{};
        wlr_output_layout_get_box(layout, out->output, &b);
        head->state.enabled = out->output->enabled;
        head->state.x = b.x;
        head->state.y = b.y;
    }
    wlr_output_manager_v1_set_configuration(output_manager, cfg);
}

namespace {

void handle_config(Server* s, wlr_output_configuration_v1* cfg, bool test_only) {
    bool ok = true;
    wlr_output_configuration_head_v1* head;
    wl_list_for_each(head, &cfg->heads, link) {
        OutputRec* out = s->output_rec(head->state.output);
        if (!out) {
            ok = false;
            continue;
        }
        wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_head_v1_state_apply(&head->state, &st);
        if (test_only) {
            // Mode changes need a buffer to test on KMS; accept what the
            // backend accepts without one, and assume mode changes work.
            if (!(st.committed & WLR_OUTPUT_STATE_MODE)) ok = ok && wlr_output_test_state(out->output, &st);
        } else {
            ok = s->apply_output_state(*out, &st) && ok;
            if (ok && head->state.enabled) wlr_output_layout_add(s->layout, out->output, head->state.x, head->state.y);
        }
        wlr_output_state_finish(&st);
    }
    if (ok) wlr_output_configuration_v1_send_succeeded(cfg);
    else wlr_output_configuration_v1_send_failed(cfg);
    wlr_output_configuration_v1_destroy(cfg);
    if (!test_only) {
        s->arrange_all();
        s->update_all_window_outputs();
        s->mark_outputs_dirty();
        s->update_output_manager();
    }
}

}  // namespace

void init_output_manager(Server* s) {
    s->output_manager = wlr_output_manager_v1_create(s->display);
    s->output_mgr_apply.connect(&s->output_manager->events.apply, [s](void* data) {
        handle_config(s, static_cast<wlr_output_configuration_v1*>(data), false);
    });
    s->output_mgr_test.connect(&s->output_manager->events.test, [s](void* data) {
        handle_config(s, static_cast<wlr_output_configuration_v1*>(data), true);
    });
    wlr_xdg_output_manager_v1_create(s->display, s->layout);
}

}  // namespace brocompositor::wl
