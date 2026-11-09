#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Quest VR tray icon (KDE Plasma's system tray, or any StatusNotifier tray).

Click to start VR: runs start-vr.sh (Monado, then SteamVR once Quest Link is accepted in the
headset) and follows its progress; the icon and tooltip show the state. The icon stays in the tray
after VR stops, like Steam. Menu: Start / Stop (stops Monado), Settings, Reset stream, USB
benchmark (Monado pads the video frames and times the sends, then suggests a bitrate), Monado's log,
controller tuning, Quit.

Needs Qt bindings for Python: Fedora `sudo dnf install python3-pyqt6` (PySide6 works too).
Install the menu entry (and optional autostart) with ./install-vr-launcher.sh.
"""

import ctypes
import fcntl
import os
import shutil
import signal
import subprocess
import sys

try:
    from PyQt6.QtCore import QProcess, QTimer
    from PyQt6.QtGui import QAction, QFontDatabase, QIcon
    from PyQt6.QtWidgets import (QApplication, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox,
                                 QFormLayout, QLabel, QLineEdit, QMenu, QPlainTextEdit, QPushButton,
                                 QSpinBox, QSystemTrayIcon, QVBoxLayout)
except ImportError:
    from PySide6.QtCore import QProcess, QTimer
    from PySide6.QtGui import QAction, QFontDatabase, QIcon
    from PySide6.QtWidgets import (QApplication, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox,
                                   QFormLayout, QLabel, QLineEdit, QMenu, QPlainTextEdit, QPushButton,
                                   QSpinBox, QSystemTrayIcon, QVBoxLayout)

HERE = os.path.dirname(os.path.realpath(__file__))
START_VR = os.path.join(HERE, "start-vr.sh")
MONADO_LOG = os.path.join(HERE, "monado-host.log")
# The tuning tool from the host build (scripts/install.sh), else a sandbox build.
TUNE = next((p for p in (os.path.join(HERE, d, "monadolink_tune") for d in ("build-host", "build"))
             if os.access(p, os.X_OK)), os.path.join(HERE, "build-host", "monadolink_tune"))
CONFIG_HOME = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
# Read by start-monado.sh (Monado) and by monadolink in SteamVR (its MONADOLINK_* keys). monadolink
# looks in ~/.config (Flatpak Steam moves XDG_CONFIG_HOME): keep the two the same if you set it.
SETTINGS = os.path.join(CONFIG_HOME, "quest-vr", "settings.env")
RESET_TRIGGER = os.path.expanduser("~/.cache/quest-vr/reset-stream")  # watched by Monado
# Monado's USB benchmark (ql_xrsp.cpp): touching the trigger (with the steps in it) starts it, the
# results come back in usb-bench.txt.
BENCH_TRIGGER = os.path.expanduser("~/.cache/quest-vr/usb-bench")
BENCH_RESULT = BENCH_TRIGGER + ".txt"
BENCH_STEPS_KB = [0, 256, 512, 1024]  # 0 = the stream as it is; 2 s each

# The streaming knobs: (env key, label, default as a string, help). Defaults = start-monado.sh's.
KNOBS = [
    ("XRT_COMPOSITOR_FORCE_GPU_INDEX", "GPU", "",
     "The GPU Monado renders and encodes on; monadolink follows it. Automatic = Monado's choice "
     "(the first discrete GPU). SteamVR picks its own GPU: with several, it must be the same one "
     "(monadolink imports its frames)."),
    ("QL_OVERRIDE_FPS", "Refresh rate", "120",
     "Quest 2: 72, 80, 90 or 120 Hz. Lower gives the encoder more time per frame."),
    ("QL_OVERRIDE_SCALE", "Resolution", "0.75",
     "Encoded size relative to the panel (1832x1920 per eye at 1.0). 0.875 still fits 120 Hz; "
     "1.0 takes ~9.4 ms per frame on the RX 6700 XT: use it with 90 Hz."),
    ("QL_FOVEATION", "Foveation", "0.75",
     "Foveated encoding: how much the centre of view is favoured over the edges. Centre sharpness "
     "is about Resolution / Foveation of the panel's: equal values (e.g. 0.75 / 0.75) = 1:1 in the "
     "centre. Lower = sharper centre or cheaper encoding, blurrier edges; 1.0 = off (even)."),
    ("XRT_COMPOSITOR_SCALE_PERCENTAGE", "Render resolution", "140",
     "The resolution games are asked to render at, relative to the stream's (Monado's default "
     "140%, which SteamVR's Auto setting raises further). ~115% keeps detail and saves a lot of GPU "
     "time: heavy games (Half-Life: Alyx) stutter when the GPU can't keep up."),
    ("QL_OVERRIDE_BITRATE_KBPS", "Bitrate", "130000",
     "Target (peaks up to twice that). Normal frames use much less: raise it if detail is "
     "limited in busy scenes."),
    ("QL_VAAPI_QMIN", "Quality floor (min QP)", "20",
     "Lower = more detail, bigger frames (USB time, hitches on scene changes). 0 = encoder default."),
    ("QL_VAAPI_ASYNC_DEPTH", "Encoder frames in flight", "1",
     "1 = lowest latency; 2 = absorbs encoder stalls for one frame more of latency."),
    ("QL_VAAPI_OPTIONS", "Extra encoder options", "",
     "ffmpeg hevc_vaapi options, key=value:key=value (advanced)."),
    ("MONADOLINK_FSR_RENDER_SCALE", "FSR render scale", "1",
     "SteamVR games: render at this fraction of the size (per side) and upscale with AMD FSR 1 in "
     "monadolink: less GPU time for any game. 1 = off; 0.77 is FSR's 'Ultra Quality'. Set SteamVR's "
     "Render Resolution to Custom 100%, or its Auto supersampling cancels part of it."),
    ("MONADOLINK_FSR_SHARPNESS", "FSR sharpness", "0.2",
     "Sharpening after the FSR upscale, in stops: 0 = sharpest, each +1 halves it."),
    ("QUEST_VR_LOGGING", "Logging", "",
     "Minimal: Monado logs errors only (no per-frame 'Slow encode' warnings or stats lines) and "
     "monadolink drops its once-a-second pose/timing lines and diagnostic texture readbacks. "
     "Saves a little CPU time on the encoder and frame threads; Normal is needed to diagnose "
     "stutter."),
]


VK_DEVICE_TYPES = {1: "integrated", 2: "discrete", 3: "virtual", 4: "CPU"}


def vulkan_gpus():
    """[(index, name, type)] in vkEnumeratePhysicalDevices order: the index Monado's
    XRT_COMPOSITOR_FORCE_GPU_INDEX takes (same loader and environment, so the same order).
    Empty if Vulkan isn't usable."""
    try:
        vk = ctypes.CDLL("libvulkan.so.1")
    except OSError:
        return []

    class InstanceCreateInfo(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32),
                    ("pApplicationInfo", ctypes.c_void_p), ("enabledLayerCount", ctypes.c_uint32),
                    ("ppEnabledLayerNames", ctypes.c_void_p), ("enabledExtensionCount", ctypes.c_uint32),
                    ("ppEnabledExtensionNames", ctypes.c_void_p)]

    vk.vkCreateInstance.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    vk.vkEnumeratePhysicalDevices.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    vk.vkGetPhysicalDeviceProperties.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    vk.vkGetPhysicalDeviceProperties.restype = None
    vk.vkDestroyInstance.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    vk.vkDestroyInstance.restype = None

    info = InstanceCreateInfo(sType=1)  # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    instance = ctypes.c_void_p()
    if vk.vkCreateInstance(ctypes.byref(info), None, ctypes.byref(instance)) != 0:
        return []
    gpus = []
    try:
        count = ctypes.c_uint32(0)
        vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None)
        devices = (ctypes.c_void_p * count.value)()
        if vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), devices) < 0:
            return []
        # VkPhysicalDeviceProperties: 5 uint32 (apiVersion, driverVersion, vendorID, deviceID,
        # deviceType), then deviceName[256], then more we don't need (the buffer is bigger than it).
        props = ctypes.create_string_buffer(4096)
        for i in range(count.value):
            vk.vkGetPhysicalDeviceProperties(devices[i], props)
            dev_type = int.from_bytes(props.raw[16:20], "little")
            name = props.raw[20:276].split(b"\0", 1)[0].decode(errors="replace")
            gpus.append((i, name, VK_DEVICE_TYPES.get(dev_type, "other")))
    finally:
        vk.vkDestroyInstance(instance, None)
    return gpus


def read_settings():
    values = {}
    try:
        with open(SETTINGS) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    values[key] = value
    except FileNotFoundError:
        pass
    return values


def write_settings(values):
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    tmp = SETTINGS + ".tmp"
    with open(tmp, "w") as f:
        f.write("# Quest VR settings (Settings window of the tray icon), read by start-monado.sh and monadolink.\n")
        for key, value in values.items():
            f.write(f"{key}={value}\n")
    os.replace(tmp, SETTINGS)


class SettingsDialog(QDialog):
    """The streaming knobs; saved to settings.env, used the next time VR starts."""

    def __init__(self, running):
        super().__init__()
        self.setWindowTitle("Quest VR settings")
        self.saved = read_settings()
        layout = QVBoxLayout(self)
        form = QFormLayout()
        layout.addLayout(form)

        self.gpu = QComboBox()
        self.gpu.addItem("Automatic", "")
        for index, name, dev_type in vulkan_gpus():
            if dev_type == "CPU":
                continue  # llvmpipe: no VAAPI encoder, far too slow
            self.gpu.addItem(f"{index}: {name} ({dev_type})", str(index))
        self.fps = QComboBox()
        for hz in ("72", "80", "90", "120"):
            self.fps.addItem(f"{hz} Hz", hz)
        self.scale = QDoubleSpinBox()
        self.scale.setRange(0.5, 1.0)
        self.scale.setSingleStep(0.025)
        self.scale.setDecimals(3)
        self.foveation = QDoubleSpinBox()
        self.foveation.setRange(0.3, 1.0)
        self.foveation.setSingleStep(0.025)
        self.foveation.setDecimals(3)
        self.render = QSpinBox()
        self.render.setRange(100, 200)
        self.render.setSingleStep(5)
        self.render.setSuffix(" % of the stream")
        self.bitrate = QSpinBox()
        self.bitrate.setRange(30, 600)
        self.bitrate.setSingleStep(10)
        self.bitrate.setSuffix(" Mbit/s")
        self.qmin = QSpinBox()
        self.qmin.setRange(0, 40)
        self.depth = QComboBox()
        self.depth.addItem("1 (lowest latency)", "1")
        self.depth.addItem("2 (absorbs stalls)", "2")
        self.options = QLineEdit()
        self.options.setPlaceholderText("e.g. rc_mode=VBR")
        self.fsr_scale = QDoubleSpinBox()
        self.fsr_scale.setRange(0.5, 1.0)
        self.fsr_scale.setSingleStep(0.05)
        self.fsr_scale.setDecimals(2)
        self.fsr_scale.valueChanged.connect(
            lambda v: self.fsr_scale.setSuffix(" (off)" if v >= 0.99 else ""))
        self.fsr_sharpness = QDoubleSpinBox()
        self.fsr_sharpness.setRange(0.0, 2.0)
        self.fsr_sharpness.setSingleStep(0.1)
        self.fsr_sharpness.setDecimals(2)
        self.logging = QComboBox()
        self.logging.addItem("Normal", "")
        self.logging.addItem("Minimal (errors only)", "minimal")
        self.widgets = {"XRT_COMPOSITOR_FORCE_GPU_INDEX": self.gpu, "QUEST_VR_LOGGING": self.logging,
                        "QL_OVERRIDE_FPS": self.fps, "QL_OVERRIDE_SCALE": self.scale,
                        "QL_FOVEATION": self.foveation, "XRT_COMPOSITOR_SCALE_PERCENTAGE": self.render,
                        "QL_OVERRIDE_BITRATE_KBPS": self.bitrate, "QL_VAAPI_QMIN": self.qmin,
                        "QL_VAAPI_ASYNC_DEPTH": self.depth, "QL_VAAPI_OPTIONS": self.options,
                        "MONADOLINK_FSR_RENDER_SCALE": self.fsr_scale,
                        "MONADOLINK_FSR_SHARPNESS": self.fsr_sharpness}
        for key, label, default, tip in KNOBS:
            widget = self.widgets[key]
            widget.setToolTip(tip)
            form.addRow(label, widget)
        self.load({k: self.saved.get(k, d) for k, _, d, _ in KNOBS})

        note = QLabel("Saved settings apply the next time VR starts."
                      + (" VR is running now: restart it to use them." if running else ""))
        note.setWordWrap(True)
        layout.addWidget(note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save
                                   | QDialogButtonBox.StandardButton.Cancel
                                   | QDialogButtonBox.StandardButton.RestoreDefaults)
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        buttons.button(QDialogButtonBox.StandardButton.RestoreDefaults).clicked.connect(
            lambda: self.load({k: d for k, _, d, _ in KNOBS}))
        layout.addWidget(buttons)

    def load(self, values):
        def pick(combo, value):
            i = combo.findData(value)
            combo.setCurrentIndex(i if i >= 0 else combo.count() - 1)
        gpu = values["XRT_COMPOSITOR_FORCE_GPU_INDEX"]
        if self.gpu.findData(gpu) < 0:  # saved GPU not found now (unplugged, driver missing): keep it
            self.gpu.addItem(f"{gpu}: (not found now)", gpu)
        self.gpu.setCurrentIndex(self.gpu.findData(gpu))
        pick(self.fps, values["QL_OVERRIDE_FPS"])
        self.scale.setValue(float(values["QL_OVERRIDE_SCALE"] or 0.75))
        self.foveation.setValue(float(values["QL_FOVEATION"] or 0.75))
        self.render.setValue(int(values["XRT_COMPOSITOR_SCALE_PERCENTAGE"] or 140))
        self.bitrate.setValue(int(int(values["QL_OVERRIDE_BITRATE_KBPS"] or 130000) / 1000))
        self.qmin.setValue(int(values["QL_VAAPI_QMIN"] or 0))
        pick(self.depth, values["QL_VAAPI_ASYNC_DEPTH"])
        self.options.setText(values["QL_VAAPI_OPTIONS"])
        self.fsr_scale.setValue(float(values["MONADOLINK_FSR_RENDER_SCALE"] or 1.0))
        self.fsr_scale.setSuffix(" (off)" if self.fsr_scale.value() >= 0.99 else "")
        self.fsr_sharpness.setValue(float(values["MONADOLINK_FSR_SHARPNESS"] or 0.2))
        self.logging.setCurrentIndex(max(0, self.logging.findData(values["QUEST_VR_LOGGING"])))

    def save(self):
        current = {
            "XRT_COMPOSITOR_FORCE_GPU_INDEX": self.gpu.currentData(),
            "QL_OVERRIDE_FPS": self.fps.currentData(),
            "QL_OVERRIDE_SCALE": f"{self.scale.value():g}",
            "QL_FOVEATION": f"{self.foveation.value():g}",
            "XRT_COMPOSITOR_SCALE_PERCENTAGE": str(self.render.value()),
            "QL_OVERRIDE_BITRATE_KBPS": str(self.bitrate.value() * 1000),
            "QL_VAAPI_QMIN": str(self.qmin.value()),
            "QL_VAAPI_ASYNC_DEPTH": self.depth.currentData(),
            "QL_VAAPI_OPTIONS": self.options.text().strip(),
            "MONADOLINK_FSR_RENDER_SCALE": f"{self.fsr_scale.value():g}",
            "MONADOLINK_FSR_SHARPNESS": f"{self.fsr_sharpness.value():g}",
            "QUEST_VR_LOGGING": self.logging.currentData(),
        }
        values = dict(self.saved)  # keep anything else in the file (e.g. QL_POSE_STATS)
        for key, label, default, tip in KNOBS:
            if current[key] == default:
                values.pop(key, None)  # defaults aren't written: start-monado.sh's own apply
            else:
                values[key] = current[key]
        write_settings(values)
        self.accept()

def parse_kv(line):
    return dict(f.split("=", 1) for f in line.split() if "=" in f)


def recommended_bitrate_mbps(mb_s, fps):
    """The bitrate whose frames fit the link: a frame may take 75% of a frame's time to send (the
    rest: audio, the headset's own messages), and VBR frames peak at about twice the average."""
    max_frame_bytes = mb_s * 1e6 * 0.75 / fps
    return max_frame_bytes / 2 * fps * 8 / 1e6


class BenchmarkDialog(QDialog):
    """USB benchmark: Monado pads the video frames to a few sizes for 2 s each and measures how long
    each frame takes to go out. Needs the stream running (Quest Link accepted)."""

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Quest VR: USB benchmark")
        self.resize(640, 380)
        layout = QVBoxLayout(self)
        info = QLabel(
            "Measures how fast video frames reach the headset over USB, with frames padded to "
            + ", ".join(f"{kb} KB" if kb else "their normal size" for kb in BENCH_STEPS_KB)
            + f" (2 s each, {2 * len(BENCH_STEPS_KB)} s in all). Keep the headset on and stay "
            "still: the picture stutters while the large frames are sent (the padding itself is "
            "invisible).")
        info.setWordWrap(True)
        layout.addWidget(info)
        self.text = QPlainTextEdit()
        self.text.setReadOnly(True)
        self.text.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        layout.addWidget(self.text)
        self.summary = QLabel("")
        self.summary.setWordWrap(True)
        layout.addWidget(self.summary)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        self.start_button = QPushButton("Start")
        self.start_button.clicked.connect(self.start)
        self.apply_button = QPushButton("Use this bitrate")
        self.apply_button.clicked.connect(self.apply_bitrate)
        self.apply_button.setVisible(False)
        buttons.addButton(self.start_button, QDialogButtonBox.ButtonRole.ActionRole)
        buttons.addButton(self.apply_button, QDialogButtonBox.ButtonRole.ActionRole)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.timer = QTimer()
        self.timer.timeout.connect(self.poll)
        self.started = 0.0
        self.recommended = None

    def start(self):
        import time
        self.start_button.setEnabled(False)
        self.apply_button.setVisible(False)
        self.summary.setText("")
        self.text.setPlainText("Asking Monado to start...")
        os.makedirs(os.path.dirname(BENCH_TRIGGER), exist_ok=True)
        self.started = time.time()
        with open(BENCH_TRIGGER, "w") as f:
            f.write(" ".join(str(kb) for kb in BENCH_STEPS_KB) + "\n")
        self.timer.start(250)

    def finish(self, message):
        self.timer.stop()
        self.start_button.setEnabled(True)
        self.start_button.setText("Run again")
        self.summary.setText(message)

    def poll(self):
        import time
        elapsed = time.time() - self.started
        try:
            fresh = os.path.getmtime(BENCH_RESULT) >= self.started - 0.5
            lines = open(BENCH_RESULT).read().splitlines() if fresh else []
        except OSError:
            lines = []
        if not lines:
            if elapsed > 3:
                self.finish("Monado didn't answer: is VR running (Quest Link accepted in the headset)? "
                            "Monado also needs a build with the benchmark (rebuild it).")
            return
        header = parse_kv(lines[0])
        fps = float(header.get("fps", 0) or 0)
        steps = [parse_kv(l) for l in lines[1:] if l.startswith("step ")]
        rows = [f"{'frame size':>12} {'frames/s':>9} {'avg KB':>7} {'send ms':>8} {'max ms':>7} "
                f"{'MB/s':>6} {'late':>5}"]
        for st in steps:
            kb = int(st["size_kb"])
            rows.append(f"{(str(kb) + ' KB') if kb else 'stream':>12} {float(st['fps']):>9.0f} "
                        f"{float(st['avg_kb']):>7.0f} {float(st['send_avg_ms']):>8.2f} "
                        f"{float(st['send_max_ms']):>7.2f} {float(st['libusb_mb_s']):>6.1f} {st['late']:>5}")
        remaining = len(BENCH_STEPS_KB) - len(steps)
        if remaining > 0 and not any(l.startswith(("done", "error=")) for l in lines):
            rows.append(f"... running ({remaining} step(s) left)")
        self.text.setPlainText(f"{header.get('codec', '?').upper()} stream at {fps:.0f} Hz\n\n"
                               + "\n".join(rows))
        error = next((l[6:] for l in lines if l.startswith("error=")), None)
        if error:
            self.finish(f"Stopped: {error}")
        elif "done" in lines:
            padded = [st for st in steps if int(st["size_kb"]) > 0 and float(st["frames"]) > 0]
            if not padded or fps <= 0:
                self.finish("No padded frames were sent.")
                return
            # The largest frames: big transfers show what the link itself takes.
            mb_s = float(max(padded, key=lambda st: float(st["avg_kb"]))["libusb_mb_s"])
            self.recommended = int(recommended_bitrate_mbps(mb_s, fps) / 5) * 5
            current = int(read_settings().get("QL_OVERRIDE_BITRATE_KBPS", "130000") or 130000) // 1000
            self.finish(
                f"USB link: about {mb_s:.0f} MB/s. At {fps:.0f} Hz a frame may take "
                f"{mb_s * 1e3 * 0.75 / fps:.0f} KB to arrive in time, so a bitrate up to about "
                f"{self.recommended} Mbit/s fits (frames peak at about twice the average). "
                f"Current setting: {current} Mbit/s."
                + (" Below 30 MB/s, try another USB port (on the back of the PC) or cable." if mb_s < 30 else ""))
            self.apply_button.setText(f"Use {self.recommended} Mbit/s")
            self.apply_button.setVisible(self.recommended != current)
        elif elapsed > 2 * len(BENCH_STEPS_KB) + 10:
            self.finish("The benchmark didn't finish (see Monado's log).")

    def apply_bitrate(self):
        values = read_settings()
        values["QL_OVERRIDE_BITRATE_KBPS"] = str(self.recommended * 1000)
        write_settings(values)
        self.apply_button.setVisible(False)
        self.summary.setText(self.summary.text() + f" Saved {self.recommended} Mbit/s: it applies the next "
                             "time VR starts.")

    def done(self, result):
        self.timer.stop()
        super().done(result)


# state -> (themed icon names, first available wins; tooltip)
STATES = {
    "idle": (["input-gaming", "applications-games"], "Quest VR: stopped (click to start)"),
    "usb": (["drive-removable-media", "input-gaming"], "Quest VR: plug in the Quest and wake it up"),
    "link": (["dialog-information", "input-gaming"], "Quest VR: put on the headset and accept Quest Link"),
    "steamvr": (["media-playback-start", "input-gaming"], "Quest VR: starting SteamVR"),
    "running": (["media-playback-start", "input-gaming"], "Quest VR: running"),
    "stopping": (["media-playback-stop", "input-gaming"], "Quest VR: stopping"),
    "error": (["dialog-error", "input-gaming"], "Quest VR: stopped with an error (see the log)"),
}


def themed_icon(names):
    for name in names:
        icon = QIcon.fromTheme(name)
        if not icon.isNull():
            return icon
    return QIcon()


def process_running(name):
    return subprocess.run(["pgrep", "-x", name], stdout=subprocess.DEVNULL).returncode == 0


class Tray:
    def __init__(self, app):
        self.app = app
        self.proc = None
        self.state = "idle"
        self.error = False

        self.tray = QSystemTrayIcon()
        self.menu = QMenu()
        self.start_action = QAction("Start VR")
        self.start_action.triggered.connect(self.start)
        self.stop_action = QAction("Stop VR (stops Monado)")
        self.stop_action.triggered.connect(self.stop)
        self.settings_action = QAction("Settings…")
        self.settings_action.triggered.connect(self.settings)
        self.reset_action = QAction("Reset stream (fresh keyframe)")
        self.reset_action.triggered.connect(self.reset_stream)
        self.bench_action = QAction("USB benchmark…")
        self.bench_action.triggered.connect(self.benchmark)
        self.bench_dialog = None
        self.log_action = QAction("Open Monado log")
        self.log_action.triggered.connect(lambda: self.open_file(MONADO_LOG))
        self.tune_action = QAction("Tune controller offset…")
        self.tune_action.triggered.connect(self.tune)
        self.quit_action = QAction("Quit")
        self.quit_action.triggered.connect(self.quit)
        for a in (self.start_action, self.stop_action):
            self.menu.addAction(a)
        self.menu.addSeparator()
        for a in (self.settings_action, self.reset_action, self.bench_action, self.log_action,
                  self.tune_action):
            self.menu.addAction(a)
        self.menu.addSeparator()
        self.menu.addAction(self.quit_action)
        self.tray.setContextMenu(self.menu)
        self.tray.activated.connect(self.activated)

        # Also notice SteamVR started or closed outside of us.
        self.poll = QTimer()
        self.poll.timeout.connect(self.refresh)
        self.poll.start(2000)

        self.set_state("idle")
        self.tray.show()

    # --- state -----------------------------------------------------------------------------------
    def set_state(self, state):
        self.state = state
        names, tip = STATES[state]
        self.tray.setIcon(themed_icon(names))
        self.tray.setToolTip(tip)
        busy = self.proc is not None
        self.start_action.setEnabled(not busy)
        self.stop_action.setEnabled(busy)
        self.tune_action.setEnabled(state == "running" and os.access(TUNE, os.X_OK))
        self.reset_action.setEnabled(state in ("steamvr", "running"))
        self.bench_action.setEnabled(state in ("steamvr", "running"))

    def refresh(self):
        if self.proc is None:
            return
        if self.state == "steamvr" and process_running("vrserver"):
            self.set_state("running")

    # --- start-vr.sh -----------------------------------------------------------------------------
    def start(self):
        if self.proc is not None:
            return
        self.error = False
        self.proc = QProcess()
        self.proc.setProgram(START_VR)
        self.proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self.output)
        self.proc.finished.connect(self.finished)
        self.proc.start()
        self.set_state("link")

    def output(self):
        text = bytes(self.proc.readAllStandardOutput()).decode(errors="replace")
        for line in text.splitlines():
            if "Plug in the Quest" in line:
                self.set_state("usb")
            elif "accept Quest Link" in line:
                self.set_state("link")
            elif "starting SteamVR" in line:
                self.set_state("steamvr")
            elif "stopping Monado" in line:
                self.set_state("stopping")
            elif "already running" in line or "stopped before" in line or "didn't start" in line:
                self.error = True

    def finished(self, code, status=None):
        self.proc = None
        self.set_state("error" if self.error or code not in (0, 130) else "idle")

    def stop(self):
        if self.proc is not None:
            self.set_state("stopping")
            # start-vr.sh's trap stops Monado (which also restores the audio output).
            os.kill(self.proc.processId(), signal.SIGTERM)

    # --- menu helpers ----------------------------------------------------------------------------
    def activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.Trigger:  # left click
            if self.proc is None:
                self.start()
            else:
                self.tray.showMessage("Quest VR", STATES[self.state][1])

    def settings(self):
        dialog = SettingsDialog(self.proc is not None)
        dialog.exec()

    def benchmark(self):
        # Not modal: VR keeps running while it measures.
        if self.bench_dialog is None:
            self.bench_dialog = BenchmarkDialog()
        self.bench_dialog.show()
        self.bench_dialog.raise_()

    def reset_stream(self):
        # Monado watches this file's modification time and sends a keyframe (see start-monado.sh).
        os.makedirs(os.path.dirname(RESET_TRIGGER), exist_ok=True)
        with open(RESET_TRIGGER, "w") as f:
            f.write("reset\n")

    def open_file(self, path):
        if os.path.exists(path):
            subprocess.Popen(["xdg-open", path])

    def tune(self):
        for term in (["konsole", "-e"], ["gnome-terminal", "--"], ["xterm", "-e"]):
            if shutil.which(term[0]):
                subprocess.Popen(term + [TUNE])
                return

    def quit(self):
        if self.proc is not None:
            self.stop()
            self.proc.waitForFinished(10000)
        self.app.quit()


def main():
    # One tray icon only.
    lock_path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "quest-vr-tray.lock")
    lock = open(lock_path, "a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        # Already in the tray: with --start (the menu entry), ask that instance to start VR.
        lock.seek(0)
        pid = lock.read().strip()
        if "--start" in sys.argv and pid.isdigit():
            os.kill(int(pid), signal.SIGUSR1)
            print("Quest VR tray is already running: asked it to start VR.")
        else:
            print("Quest VR tray is already running.")
        return 0
    lock.seek(0)
    lock.truncate()
    lock.write(str(os.getpid()))
    lock.flush()

    app = QApplication(sys.argv)
    app.setApplicationName("Quest VR")
    app.setQuitOnLastWindowClosed(False)
    if not QSystemTrayIcon.isSystemTrayAvailable():
        print("No system tray available.", file=sys.stderr)
    tray = Tray(app)  # noqa: F841 (kept alive)
    if "--start" in sys.argv:
        QTimer.singleShot(0, tray.start)
    # SIGUSR1 from a second launch: start VR. Python runs signal handlers between bytecodes, so a
    # short timer keeps the interpreter ticking while Qt's event loop waits.
    signal.signal(signal.SIGUSR1, lambda *_: QTimer.singleShot(0, tray.start))
    signal.signal(signal.SIGTERM, lambda *_: QTimer.singleShot(0, tray.quit))
    ticker = QTimer()
    ticker.timeout.connect(lambda: None)
    ticker.start(250)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
