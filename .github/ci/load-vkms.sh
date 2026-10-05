#!/usr/bin/env bash
# Loads vkms (the kernel's virtual KMS driver) on the runner host and writes
# its card node to $GITHUB_OUTPUT as `device`, for test_wl_drm. When the
# runner's kernel has no vkms module, writes nothing: the test then skips, and
# says so.
set -uo pipefail

if ! sudo modprobe vkms 2>/dev/null; then
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends "linux-modules-extra-$(uname -r)" >/dev/null 2>&1
    sudo modprobe vkms 2>/dev/null || echo "vkms is not available on kernel $(uname -r); test_wl_drm will skip"
fi

# vkms registers a bare platform (or faux) device named "vkms" with no driver
# bound to it, so the card is recognised by its device's name.
ls -l /sys/class/drm/ 2>/dev/null
for card in /sys/class/drm/card[0-9] /sys/class/drm/card[0-9][0-9]; do
    [ -e "$card/device" ] || continue
    if [ "$(basename "$(readlink -f "$card/device")")" = vkms ]; then
        dev="/dev/dri/$(basename "$card")"
        echo "vkms card: $dev"
        sudo chmod a+rw "$dev"
        echo "device=$dev" >> "${GITHUB_OUTPUT:-/dev/null}"
        exit 0
    fi
done
exit 0
