#!/usr/bin/env bash
# The whole Linux build, Wayland server included, inside debian:trixie (which
# packages wlroots 0.18 as libwlroots-0.18-dev). Called by CI as the container's
# command with the workspace mounted at /w:
#
#   /w/brocompositor           this repo
#   /w/brodisplays             the sibling, unless SOURCE=submodule
#
# Environment: CC / CXX (gcc|clang), CONFIG (Release|Debug), COVERAGE (ON|OFF),
# BROCOMPOSITOR_DRM_DEVICE (a vkms card the host loaded, or empty).
#
# Builds as root, then runs ctest as an unprivileged user in the video group:
# the servers and clients the tests start are the ones a desktop user runs.
set -euo pipefail

: "${CC:=gcc}" "${CXX:=g++}" "${CONFIG:=Release}" "${COVERAGE:=OFF}"
export LANG=C.UTF-8
cd /w

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# Build: wlroots and the protocol tooling, the Vulkan headers for the importer,
# xcb / xcb-randr / xau (+ gio) for brodisplays in test_wl_brodisplays.
build_packages=(
    ca-certificates git cmake ninja-build pkg-config g++ clang
    libwlroots-0.18-dev wayland-protocols libwayland-dev libxkbcommon-dev libpixman-1-dev
    libdrm-dev libgbm-dev libvulkan-dev libxcb1-dev libxcb-randr0-dev libxau-dev libglib2.0-dev
)
# Test: the third-party clients and servers the tests drive (each test skips
# what is missing, so this list is what makes them run rather than skip), and
# lavapipe so test_wl_vulkan has a Vulkan device without a GPU.
test_packages=(
    xwayland xvfb xterm x11-apps xclip xsel
    weston foot wl-clipboard wlr-randr gtk-3-examples qt6-base-examples qt6-wayland
    swaylock swayidle grim wtype wlrctl wlsunset seatd
    dbus dbus-user-session mesa-vulkan-drivers libgl1-mesa-dri libegl-mesa0 fonts-dejavu-core
    xkb-data
)
extra=()
[ "$COVERAGE" = "ON" ] && extra+=(gcovr)
apt-get install -y --no-install-recommends "${build_packages[@]}" "${test_packages[@]}" "${extra[@]}"
pkg-config --modversion wlroots-0.18

configure_args=(-S brocompositor -B build -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG")
[ "$COVERAGE" = "ON" ] && configure_args+=(-DBROCOMPOSITOR_COVERAGE=ON)
cmake "${configure_args[@]}" | tee configure.log
grep -q 'BROCOMPOSITOR_WITH_WAYLAND:BOOL=ON' build/CMakeCache.txt
cmake --build build --parallel "$(nproc)"

id ci >/dev/null 2>&1 || useradd -m -G video ci
chown -R ci /w

# test_wl_drm takes DRM master on the vkms card through seatd.
seatd_pid=""
if [ -n "${BROCOMPOSITOR_DRM_DEVICE:-}" ] && [ -e "$BROCOMPOSITOR_DRM_DEVICE" ]; then
    seatd -g video >/tmp/seatd.log 2>&1 &
    seatd_pid=$!
    for _ in $(seq 50); do [ -S /run/seatd.sock ] && break; sleep 0.1; done
    export LIBSEAT_BACKEND=seatd
else
    unset BROCOMPOSITOR_DRM_DEVICE
fi

set +e
runuser -u ci -- env \
    ${BROCOMPOSITOR_DRM_DEVICE:+BROCOMPOSITOR_DRM_DEVICE="$BROCOMPOSITOR_DRM_DEVICE"} \
    ${LIBSEAT_BACKEND:+LIBSEAT_BACKEND="$LIBSEAT_BACKEND"} \
    /w/brocompositor/.github/ci/ctest.sh --test-dir build
rc=$?
set -e
[ -n "$seatd_pid" ] && kill "$seatd_pid" 2>/dev/null

# Which Vulkan devices the dmabuf import test exercised (lavapipe here).
echo
sed -n '/Testing: test_wl_vulkan/,/<end of output>/p' build/Testing/Temporary/LastTest.log | grep -E '^ *--|texel|output images' || true

# Scoped to brocompositor's own src/ and include/: brodisplays is measured by
# its own CI, the tests are not the subject, and the wayland-scanner output is
# generated. pipefail is on, so a gcovr crash fails the job.
if [ "$COVERAGE" = "ON" ]; then
    mkdir -p coverage-html
    gcovr --root . \
        --filter 'brocompositor/src/' --filter 'brocompositor/include/brocompositor/' \
        --exclude-unreachable-branches \
        --print-summary \
        --html-details coverage-html/index.html \
        | tee coverage-summary.txt
    test -s coverage-html/index.html
fi

chmod -R a+rwX /w
exit "$rc"
