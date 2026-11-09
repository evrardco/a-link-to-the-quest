#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Start monado-service with the Quest Link driver, tuned for VAAPI (GPU) HEVC encoding at 120 Hz.
# Works in a sandbox (external/monado/build) and on the Fedora host (external/monado/build-fedora).
# Every setting can be overridden from the environment, e.g. QL_OVERRIDE_FPS=72 ./start-monado.sh
# Saved settings (the Quest VR tray's Settings window) come from ~/.config/quest-vr/settings.env
# (KEY=VALUE lines); the environment still wins over them, and they win over the defaults below.
set -euo pipefail
cd "$(dirname "$0")"

SETTINGS=${XDG_CONFIG_HOME:-$HOME/.config}/quest-vr/settings.env
if [ -f "$SETTINGS" ]; then
    while IFS='=' read -r key value || [ -n "$key" ]; do
        case "$key" in
        QL_* | XRT_* | QUEST_VR_LOGGING | MONADO_TREE) ;; # only Monado's settings
        *) continue ;;
        esac
        if [ -z "${!key+x}" ]; then
            export "$key=$value"
        fi
    done <"$SETTINGS"
fi

# Only used by the x264 (CPU) fallback, which defaults to one encoder thread.
export QL_THREADS_PER_SLICE=${QL_THREADS_PER_SLICE:-8}
# Quest 2 supports 72/80/90/120 Hz. With VAAPI (GPU) encoding, 120 Hz was the smoothest
# (0 compositor misses, fewer encoder stalls than 90 Hz); use 90 for the x264 fallback.
export QL_OVERRIDE_FPS=${QL_OVERRIDE_FPS:-120}
# Render scale relative to the panel (driver default 0.75 on USB 3).
export QL_OVERRIDE_SCALE=${QL_OVERRIDE_SCALE:-0.75}
# Stream codec: hevc (default) or h264. Sets both the VAAPI encoder and what the handshake announces.
export QL_VIDEO_CODEC=${QL_VIDEO_CODEC:-hevc}
# VAAPI frames in flight. 1 = lowest latency (encode ~5 ms instead of ~9): judged "really great" on
# 2026-10-29 once monadolink stopped its early-wait experiment; occasional ~12 ms encoder stalls still
# cost a frame each. 2 absorbs those stalls for ~1 frame of latency.
export QL_VAAPI_ASYNC_DEPTH=${QL_VAAPI_ASYNC_DEPTH:-1}
# VAAPI minimum QP (build default 20): bounds the huge frames after scene changes (0 = encoder
# default). QL_VAAPI_VBV_FRAMES=N caps them harder (CBR, but pads every frame). See VideoEncoderVA.cpp.
# VAAPI keyframe interval in frames: unset = the build's default (CMake QL_VAAPI_GOP_DEFAULT, 432000,
# i.e. keyframes only at stream start). ffmpeg's old default of 120 caused a late frame every second.
# Example: QL_VAAPI_GOP=120 ./start-monado.sh
# Pose prediction from the headset's velocities (1, the build default); 0 = none, as before 2026-10-29.
# QL_POSE_STATS=1 logs pose rate/age and checks the velocities every 2 s.
# Render resolution games are asked for, in % of the stream's (Monado's default 140; SteamVR's Auto
# raises it further). Lower saves GPU time in heavy games (e.g. 115). Only the recommended size:
# Monado's compute compositor doesn't use it otherwise.
export XRT_COMPOSITOR_SCALE_PERCENTAGE=${XRT_COMPOSITOR_SCALE_PERCENTAGE:-140}
export XRT_COMPOSITOR_COMPUTE=${XRT_COMPOSITOR_COMPUTE:-1}
# QUEST_VR_LOGGING=minimal (the tray's Logging setting): errors only, so no per-frame "Slow encode/
# PushFrame/VAAPI frame" warnings or stats lines written from the encoder and compositor threads.
# A *_LOG set in the environment still wins.
if [ "${QUEST_VR_LOGGING:-}" = minimal ]; then
    for v in XRT_LOG QUEST_LINK_LOG XRT_COMPOSITOR_LOG U_PACING_COMPOSITOR_LOG U_PACING_APP_LOG IPC_LOG; do
        if [ -z "${!v+x}" ]; then export "$v=error"; fi
    done
    echo "Minimal logging: Monado logs errors only"
fi
export XRT_LOG=${XRT_LOG:-info}
# Upstream Monado (external/monado) creates its devices and compositor when the first app
# connects; with Quest Link that app then waits inside Monado until Link is accepted (SteamVR's driver
# watchdog gives up after 20 s). Our port's IPC_EAGER_SYSTEM creates them first and only then opens the
# socket, like the 2023 Quest Link fork did, so start-vr.sh can wait for the socket.
export IPC_EAGER_SYSTEM=${IPC_EAGER_SYSTEM:-1}

# Which Monado: external/monado (the submodule: upstream Monado with the Quest Link driver), or
# MONADO_TREE=<path> for another checkout (e.g. ../monado). "monado-upstream" and "upstream" are old
# names for the submodule. build/ is a Debian-sandbox build; on Fedora it's build-monado-fedora.sh's
# build-fedora/.
case "${MONADO_TREE:-}" in
"" | monado-upstream | upstream) MONADO_TREE=external/monado ;;
esac
if [ -f /etc/fedora-release ]; then
    BIN=${MONADO_BIN:-$MONADO_TREE/build-fedora/src/xrt/targets/service/monado-service}
else
    BIN=${MONADO_BIN:-$MONADO_TREE/build/src/xrt/targets/service/monado-service}
fi
echo "Monado: $BIN"

# monadolink (in SteamVR) must load the OpenXR client of this same Monado build: a client from the
# other tree fails the IPC handshake. It reads this copy of the manifest (absolute library paths).
RUNTIME_FILE=$HOME/.cache/quest-vr/openxr_runtime.json
MANIFEST=${BIN%/src/xrt/targets/service/monado-service}/openxr_monado-dev.json
if [ -f "$MANIFEST" ]; then
    mkdir -p "$(dirname "$RUNTIME_FILE")"
    cp "$MANIFEST" "$RUNTIME_FILE.tmp" && mv -f "$RUNTIME_FILE.tmp" "$RUNTIME_FILE"
else
    echo "Note: no $MANIFEST: monadolink keeps its configured runtime (it must match this Monado)" >&2
fi

# Without CAP_SYS_NICE Monado's compositor gets a normal-priority GPU queue (same as the game) and no
# realtime threads, so a game that saturates the GPU makes frames late. A rebuild drops it.
if command -v getcap >/dev/null && ! getcap "$BIN" 2>/dev/null | grep -q cap_sys_nice; then
    echo "Note: $BIN has no CAP_SYS_NICE (normal GPU priority): run ./grant-vr-priority.sh once (after each rebuild)." >&2
fi

# A previous instance can take a few seconds to shut down (longer with a client attached);
# starting a second one aborts with "monado-service is already running!".
for _ in $(seq 20); do
    pgrep -x monado-service >/dev/null || break
    sleep 0.5
done
if pgrep -x monado-service >/dev/null; then
    echo "monado-service is still running (pid $(pgrep -x monado-service)); stop it first." >&2
    exit 1
fi

# Without a headset Monado falls back to a simulated HMD and crashes in its no-window path,
# so wait until a Meta (2833) USB device is present.
quest_present() { grep -qx 2833 /sys/bus/usb/devices/*/idVendor 2>/dev/null; }
if ! quest_present; then
    echo "Waiting for the Quest on USB (plug it in and wake it up; Ctrl+C to cancel)..."
    until quest_present; do sleep 1; done
    sleep 2 # let it finish enumerating
fi
echo "Quest found: $(for f in /sys/bus/usb/devices/*/idVendor; do d=${f%/idVendor}; [ "$(cat "$f")" = 2833 ] && echo "$(cat "$d/idProduct") at $(cat "$d/speed") Mbit/s"; done | head -1)"

# Sound in the headset: Monado streams the "Quest-Link" output (a null sink; Monado records its
# monitor). It becomes the default output while Monado runs; the previous one comes back after.
# QL_AUDIO=0: no sink (sound stays on the PC); QL_AUDIO_DEFAULT_SINK=0: create it, don't switch to it.
# The headset's microphone: Monado plays it into the "quest_link_mic_sink" null sink, whose monitor
# is remapped as the "Quest-Link Microphone" input (the default input while Monado runs).
# QL_MIC=0: none; QL_MIC_DEFAULT_SOURCE=0: create it, don't switch to it.
QL_AUDIO=${QL_AUDIO:-1}
QL_AUDIO_DEFAULT_SINK=${QL_AUDIO_DEFAULT_SINK:-1}
QL_MIC=${QL_MIC:-1}
QL_MIC_DEFAULT_SOURCE=${QL_MIC_DEFAULT_SOURCE:-1}
export QL_AUDIO QL_MIC
audio_modules=()
audio_previous=""
mic_previous=""
audio_restore() {
    if [ -n "$audio_previous" ]; then
        pactl set-default-sink "$audio_previous" && echo "Audio: default output restored to $audio_previous"
    fi
    if [ -n "$mic_previous" ]; then
        pactl set-default-source "$mic_previous" && echo "Audio: default input restored to $mic_previous"
    fi
    local m
    for m in "${audio_modules[@]}"; do
        pactl unload-module "$m"
    done
}
load_module() { # load_module <args...>: remembers the module to unload it when Monado exits
    local m
    m=$(pactl load-module "$@") && audio_modules=("$m" "${audio_modules[@]}")
}
if { [ "$QL_AUDIO" != 0 ] || [ "$QL_MIC" != 0 ]; } && command -v pactl >/dev/null && pactl info >/dev/null 2>&1; then
    if [ "$QL_AUDIO" != 0 ]; then
        if ! pactl list short sinks | grep -qw quest_link; then
            load_module module-null-sink sink_name=quest_link sink_properties=device.description=Quest-Link
        fi
        current=$(pactl get-default-sink)
        if [ "$QL_AUDIO_DEFAULT_SINK" != 0 ] && [ "$current" != quest_link ]; then
            pactl set-default-sink quest_link && audio_previous=$current
            echo "Audio: default output is Quest-Link while Monado runs (was $current)"
        fi
    fi
    if [ "$QL_MIC" != 0 ]; then
        if ! pactl list short sinks | grep -qw quest_link_mic_sink; then
            load_module module-null-sink sink_name=quest_link_mic_sink rate=48000 channels=1 \
                sink_properties=device.description=Quest-Link-Mic-Feed
        fi
        if ! pactl list short sources | grep -qw quest_link_mic; then
            load_module module-remap-source master=quest_link_mic_sink.monitor source_name=quest_link_mic \
                source_properties=device.description=Quest-Link-Microphone
        fi
        current=$(pactl get-default-source)
        if [ "$QL_MIC_DEFAULT_SOURCE" != 0 ] && [ "$current" != quest_link_mic ]; then
            pactl set-default-source quest_link_mic && mic_previous=$current
            echo "Audio: default input is Quest-Link-Microphone while Monado runs (was $current)"
        fi
    fi
    trap audio_restore EXIT
    trap 'exit 130' INT TERM # so the EXIT trap runs (after Monado has stopped)
elif [ "$QL_AUDIO" != 0 ] || [ "$QL_MIC" != 0 ]; then
    echo "Audio: no pactl/PulseAudio server here: sound and the mic stay on this machine"
fi

# monado-service watches stdin for a quit key and exits if stdin can't be polled
# (e.g. /dev/null when run in the background), so give it a pipe when stdin isn't a terminal.
# Not exec'd: the audio settings are restored after it exits.
if [ -t 0 ]; then
    "$BIN"
else
    "$BIN" < <(exec sleep infinity)
    kill $! 2>/dev/null || true
fi
