#!/usr/bin/env bash
# Loads the kernel's virtual devices on the runner host for the container the
# Wayland tests run in, and writes to $GITHUB_OUTPUT:
#
#   drm_device   the vkms card node (test_wl_drm takes DRM master on it)
#   docker_args  the --device options that hand the nodes to the container
#
# vkms is a virtual KMS driver; udmabuf lets lavapipe allocate dmabufs, which
# test_wl_vulkan imports. A module the runner's kernel lacks is reported and
# left out, and the tests that need it skip and say so.
set -uo pipefail

load() {
    sudo modprobe "$1" 2>/dev/null && return 0
    if [ -z "${extra_installed:-}" ]; then
        sudo apt-get update -qq
        sudo apt-get install -y --no-install-recommends "linux-modules-extra-$(uname -r)" >/dev/null 2>&1
        extra_installed=1
    fi
    sudo modprobe "$1" 2>/dev/null || { echo "$1 is not available on kernel $(uname -r)"; return 1; }
}

docker_args=""
drm_device=""

if load vkms; then
    # vkms registers a bare platform (or faux) device named "vkms" with no
    # driver bound to it, so the card is recognised by its device's name.
    for card in /sys/class/drm/card[0-9] /sys/class/drm/card[0-9][0-9]; do
        [ -e "$card/device" ] || continue
        if [ "$(basename "$(readlink -f "$card/device")")" = vkms ]; then
            drm_device="/dev/dri/$(basename "$card")"
            sudo chmod a+rw "$drm_device"
            docker_args="--device $drm_device"
            echo "vkms card: $drm_device"
        fi
    done
fi

if load udmabuf && [ -e /dev/udmabuf ]; then
    sudo chmod a+rw /dev/udmabuf
    docker_args="$docker_args --device /dev/udmabuf"
    echo "udmabuf: /dev/udmabuf"
fi

{
    echo "drm_device=$drm_device"
    echo "docker_args=$docker_args"
} >> "${GITHUB_OUTPUT:-/dev/null}"
exit 0
