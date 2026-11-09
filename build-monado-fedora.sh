#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Monado with the Quest Link driver (external/monado, VAAPI encoding) on Fedora and set up USB access.
# Run on the Fedora host, as your normal user (it calls sudo where needed).
set -euo pipefail
cd "$(dirname "$0")"

# RPM Fusion (free): x264-devel, full ffmpeg, and Mesa's VAAPI driver with H.264/HEVC encoding
# (Fedora's own mesa-va-drivers has those codecs stripped for patent reasons).
if ! rpm -q rpmfusion-free-release >/dev/null 2>&1; then
    sudo dnf install -y "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
fi
rpm -q ffmpeg >/dev/null 2>&1 || sudo dnf swap -y ffmpeg-free ffmpeg --allowerasing
rpm -q mesa-va-drivers-freeworld >/dev/null 2>&1 || sudo dnf swap -y mesa-va-drivers mesa-va-drivers-freeworld

sudo dnf install -y --skip-unavailable \
    cmake ninja-build gcc-c++ git pkgconf-pkg-config \
    eigen3-devel vulkan-loader-devel vulkan-headers glslang \
    libX11-devel libXxf86vm-devel libXrandr-devel libxcb-devel \
    libglvnd-devel mesa-libGL-devel mesa-libEGL-devel wayland-devel wayland-protocols-devel libdrm-devel \
    libusb1-devel systemd-devel hidapi-devel libuvc-devel cjson-devel SDL2-devel \
    libbsd-devel dbus-devel libjpeg-turbo-devel zlib-ng-compat-devel \
    capnproto capnproto-devel json-devel x264-devel \
    ffmpeg-devel libva-devel libva-utils \
    pulseaudio-libs-devel pulseaudio-utils \
    android-tools vulkan-tools

# The stream is encoded on the GPU; make sure the driver can actually do it.
if ! vainfo 2>/dev/null | grep -q "VAProfileHEVCMain.*VAEntrypointEncSlice"; then
    echo "WARNING: vainfo shows no HEVC encode support; streaming will fail. Check mesa-va-drivers-freeworld." >&2
fi

# Give the logged-in user (not every account) access to Meta (2833) USB devices, any product ID.
# Newer firmware enumerates as 2833:5010 etc., so don't list product IDs.
RULE=/etc/udev/rules.d/70-oculus.rules
if [ ! -f "$RULE" ]; then
    echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="2833", TAG+="uaccess"' | sudo tee "$RULE" >/dev/null
    sudo udevadm control --reload
    echo "udev rule installed: replug the headset."
fi

# Monado with the Quest Link driver: the submodule external/monado (github.com/evrardco/monado-quest,
# branch oculus-quest-2). `./build-monado-fedora.sh upstream` still works (the old name for it).
TREE=external/monado
if [ ! -f "$TREE/CMakeLists.txt" ]; then
    ./init.sh # a fresh clone: fetch the submodules
fi

# build-fedora/, so a sandbox build (build/) can sit next to it.
# Bundled cJSON: monadolink loads Monado's OpenXR client inside SteamVR's container (the Steam Linux
# Runtime), which has no libcjson.so.1 ("cannot open shared object file", 2026-11-09).
cmake -S "$TREE" -B "$TREE/build-fedora" -G Ninja \
    -DXRT_ENABLE_GPL=ON -DXRT_BUILD_DRIVER_QUEST_LINK=ON -DXRT_HAVE_SYSTEM_CJSON=OFF \
    -DCMAKE_BUILD_TYPE="${MONADO_BUILD_TYPE:-RelWithDebInfo}" # -O2 with symbols (readable backtraces); MONADO_BUILD_TYPE=Debug to debug
grep -q "^XRT_HAVE_FFMPEG:BOOL=ON" "$TREE/build-fedora/CMakeCache.txt" \
    || echo "WARNING: ffmpeg not found by CMake; Monado will fall back to x264 (CPU) encoding." >&2
grep -Eq "^XRT_BUILD_DRIVER_QUEST_LINK:BOOL=(ON|1)$" "$TREE/build-fedora/CMakeCache.txt" \
    || { echo "ERROR: the Quest Link driver isn't enabled (a dependency is missing: see CMake's output above)" >&2; exit 1; }
ninja -C "$TREE/build-fedora"
# The new binary lost its CAP_SYS_NICE: let Monado's compositor outrank games on the GPU again.
./grant-vr-priority.sh --monado-only

echo
echo "Built Monado. Start VR with ./start-vr.sh or the tray icon (./install-vr-launcher.sh), or Monado alone with"
echo "./start-monado.sh."
echo "Then install the SteamVR driver:  scripts/install.sh"
