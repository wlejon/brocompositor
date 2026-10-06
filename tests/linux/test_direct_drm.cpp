// Direct DRM/KMS backend unit tests: pipeline discovery, mode synthesis,
// device management and direct DRM output interface.
#include "check.h"
#include "linux/drm/drm_device.h"
#include "linux/drm/drm_output.h"
#include "linux/drm_util.h"

#include <cstring>

using namespace brocompositor;
using namespace brocompositor::wl;
using namespace brocompositor::wl::drm;

void test_mode_synthesis() {
    drmModeModeInfo mode = make_mode_info(1920, 1080, 60000);
    CHECK_EQ(mode.hdisplay, 1920u);
    CHECK_EQ(mode.vdisplay, 1080u);
    CHECK_EQ(mode.vrefresh, 60u);
    CHECK(mode.htotal > mode.hdisplay);
    CHECK(mode.vtotal > mode.vdisplay);
    CHECK(std::strcmp(mode.name, "1920x1080") == 0);
}

void test_card_discovery() {
    std::string card = find_drm_card_path();
    // A machine with DRM cards should find card0 or card1.
    if (!card.empty()) {
        std::printf("Found DRM card: %s\n", card.c_str());
        CHECK(card.find("/dev/dri/card") != std::string::npos);
    }
}

void test_drm_output_recognition() {
    CHECK(!is_direct_drm_output(nullptr));
    wlr_output fake{};
    CHECK(!is_direct_drm_output(&fake));
    CHECK(!is_drm_output(&fake));
}

void test_kms_pipeline_discovery() {
    std::string card = find_drm_card_path();
    if (card.empty()) return;

    auto kms_res = brodmabuf::KmsDevice::open(card);
    if (!kms_res.ok()) return;

    auto& dev = *kms_res.value();
    if (!dev.is_atomic_supported()) return;

    auto pipelines = discover_drm_pipelines(dev);
    std::printf("Discovered %zu display pipeline(s) on %s\n", pipelines.size(), card.c_str());
    for (const auto& pipe : pipelines) {
        std::printf("  Pipeline: connector %u (%s), crtc %u, plane %u, mode %s (%ux%u@%uHz)\n",
                    pipe.connector_id, pipe.name.c_str(), pipe.crtc_id, pipe.plane_id,
                    pipe.mode.name, pipe.mode.hdisplay, pipe.mode.vdisplay, pipe.mode.vrefresh);
        CHECK(pipe.connector_id != 0u);
        CHECK(pipe.crtc_id != 0u);
        CHECK(pipe.plane_id != 0u);
        CHECK(!pipe.modes.empty());
        CHECK(pipe.plane_props.fb_id != 0u);
        CHECK(pipe.crtc_props.mode_id != 0u);
        CHECK(pipe.crtc_props.active != 0u);
    }
}

int main() {
    test_mode_synthesis();
    test_card_discovery();
    test_drm_output_recognition();
    test_kms_pipeline_discovery();
    return bctest::finish("test_direct_drm");
}
