# A Link to the Quest

PC VR on Linux with a **Meta Quest 2 over USB**, without Meta's software: SteamVR games stream to
the headset through [Monado](https://monado.freedesktop.org/) and its reverse-engineered Quest Link
driver.

```
Quest 2 ──USB (Quest Link / XRSP)── Monado ──OpenXR── monadolink ── SteamVR ── your games
                                    (external/monado)  (this repo's SteamVR driver)
```

* **Monado** (`external/monado`, a submodule): upstream Monado with the Quest Link driver ported onto
  it. It talks to the headset, encodes the video on the GPU (VAAPI, HEVC) and streams it over USB.
* **monadolink** (`src/`): a SteamVR driver that appears to SteamVR as a headset with Touch
  controllers and hands SteamVR's frames to Monado.
* **Quest VR tray icon** (`vr-tray.py`) and scripts: start everything with one click, and change
  the streaming settings.

## DISCLAIMER

**This project was vibe coded.** It was written almost entirely by an AI coding assistant (Claude),
steered and tested by a single user on a single machine (Fedora 44, AMD RX 6700 XT, Meta Quest 2).
It has not been reviewed by the Monado developers or anyone else. It is not affiliated with or
endorsed by Meta, Valve or the Monado project. Expect bugs; use it at your own risk.

VR can make you feel sick when a pose, the field of view or the timing is wrong. Try a new build with
the headset in your hand before wearing it, and take it off at once if something feels off.

## Requirements

* **Fedora** on the PC: the build and setup scripts use `dnf` and RPM Fusion.
* **A GPU with VAAPI HEVC encoding.** Only an AMD Radeon RX 6700 XT (Mesa) has been tested. Fedora's
  own Mesa has HEVC/H.264 encoding removed; the build script installs RPM Fusion's.
* **A Meta Quest 2** with its USB-C cable, on a **USB 3** port and a USB 3 cable: USB 2 can't carry
  the stream. Other Quest models are untested.
* **Steam with SteamVR** (the native RPM or the Flatpak).
* For the tray icon: `python3-pyqt6` (`sudo dnf install python3-pyqt6`).

## Build and install

All of this runs on the PC, as your normal user (the scripts call `sudo` where they need it).

```sh
git clone https://github.com/evrardco/a-link-to-the-quest.git
cd a-link-to-the-quest
./init.sh                   # fetch the submodules (Monado, OpenVR headers, ...)
./build-monado-fedora.sh    # install the dependencies, a udev rule for the Quest, build Monado
scripts/install.sh          # build monadolink and install it into SteamVR's drivers
./install-vr-launcher.sh    # add "Quest VR" to the application menu (--autostart: tray icon at login)
```

* `build-monado-fedora.sh` installs the build dependencies (RPM Fusion's ffmpeg and Mesa VAAPI
  drivers included) and the udev rule that lets you use the Quest's USB device: unplug and replug
  the headset after the first run. It then builds Monado into `external/monado/build-fedora`, and
  gives `monado-service` the `CAP_SYS_NICE` capability (with `sudo`), so Monado's compositor gets a
  high-priority GPU queue. A rebuild loses it: the script grants it again.
* `./grant-vr-priority.sh` does the same for SteamVR's compositor; run it again after SteamVR
  updates.
* SteamVR runs inside the Steam Linux Runtime container. If monadolink can't see Monado's socket
  (SteamVR's log, `~/.local/share/Steam/logs/vrserver.txt`, says "Monado socket ... is NOT visible
  here"), set **SteamVR's** launch options (Steam → Library → SteamVR → Properties) to
  `PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc %command%`.

To update later: `git pull && ./init.sh`, then rebuild with `./build-monado-fedora.sh` and
`scripts/install.sh`.

## Using it

1. Click **Quest VR** in the application menu, or the tray icon.
2. Plug in the Quest and wake it up when asked (desktop notifications show each step).
3. In the headset, **accept Quest Link** (it's in the Quest's quick settings).
4. SteamVR starts on its own once Link is up. Start your game from Steam as usual.
5. When you close SteamVR, Monado stops too, and your PC's sound output is switched back.

Sound plays in the headset while VR runs: the scripts create a "Quest-Link" output, make it the
default and switch back afterwards. The headset's microphone becomes the "Quest-Link Microphone"
input.

Without the tray icon: `./start-vr.sh` does the same from a terminal. `./start-monado.sh` starts
Monado alone (for OpenXR apps, or to test).

### The tray icon

The icon shows where things are: stopped, waiting for the USB connection, waiting for you to accept
Link, starting SteamVR, running, or stopped with an error. Its menu:

| Menu item | What it does |
|---|---|
| **Start VR** / **Stop VR** | Start everything / stop Monado (SteamVR loses the headset). |
| **Settings…** | The streaming settings below. Saved settings apply the next time VR starts. |
| **Reset stream** | Ask for a fresh keyframe: clears a smear or noise that stays in the picture. |
| **USB benchmark…** | Measures how fast frames reach the headset (about 8 s, the picture stutters meanwhile) and suggests a bitrate. Needs VR running. |
| **Open Monado log** | `monado-host.log`, the first place to look when something goes wrong. |
| **Tune controller offset…** | A terminal tool to adjust where SteamVR places the controllers. |
| **Quit** | Remove the tray icon (stops VR if it runs). |

### In the headset

Hold **both thumbsticks clicked for one second** to enter (long buzz) or leave (short buzz) the
tuning mode. While in it, games don't see your buttons:

| Input | Effect |
|---|---|
| Right stick | Aim the controllers up/down, left/right (1° per flick) |
| Right grip held + right stick | Raise/lower (5 mm), twist (1°) |
| Left stick | Move the controllers forward/back, left/right (5 mm per flick) |
| **A** | Controller offset back to the defaults |
| **B** | Reset the video stream (fresh keyframe) |
| **X** | Put one controller on the floor, then press X: sets the floor height |
| **Y** | Back to the headset's own floor |

## Settings

### Streaming (tray → Settings…)

Saved in `~/.config/quest-vr/settings.env` (`KEY=VALUE` lines; you can edit it by hand). They apply
the next time VR starts; a variable set in the environment wins over the file.

| Setting | Key | Default | Meaning |
|---|---|---|---|
| GPU | `XRT_COMPOSITOR_FORCE_GPU_INDEX` | automatic | The GPU Monado renders and encodes on (monadolink follows it). |
| Refresh rate | `QL_OVERRIDE_FPS` | 120 | 72, 80, 90 or 120 Hz. Lower gives the encoder more time per frame. |
| Resolution | `QL_OVERRIDE_SCALE` | 0.75 | Streamed size relative to the panel (1832×1920 per eye at 1.0). 1.0 is best used at 90 Hz. |
| Foveation | `QL_FOVEATION` | 0.75 | How much the centre of the view gets more pixels than the edges. 1.0 = off. |
| Render resolution | `XRT_COMPOSITOR_SCALE_PERCENTAGE` | 140 | What games are asked to render at, in % of the stream. ~115 % saves a lot of GPU time. |
| Bitrate | `QL_OVERRIDE_BITRATE_KBPS` | 130000 | Target bitrate (frames peak at about twice it). The USB benchmark tells you what your link takes. |
| Quality floor | `QL_VAAPI_QMIN` | 20 | Lower = more detail but bigger frames (USB time, hitches on scene changes). |
| Encoder frames in flight | `QL_VAAPI_ASYNC_DEPTH` | 1 | 1 = lowest latency; 2 absorbs encoder stalls for one more frame of latency. |
| Extra encoder options | `QL_VAAPI_OPTIONS` | | ffmpeg `hevc_vaapi` options, `key=value:key=value` (advanced). |
| FSR render scale | `MONADOLINK_FSR_RENDER_SCALE` | 1 (off) | SteamVR games render smaller and monadolink upscales with AMD FSR 1. 0.77 = FSR's "Ultra Quality". |
| FSR sharpness | `MONADOLINK_FSR_SHARPNESS` | 0.2 | Sharpening after the upscale: 0 = sharpest. |
| Logging | `QUEST_VR_LOGGING` | normal | `minimal`: errors only. Keep it normal when chasing stutter. |

Only in the file or the environment:

| Key | Meaning |
|---|---|
| `QL_AUDIO=0`, `QL_MIC=0` | Keep the sound / the microphone on the PC. |
| `QL_AUDIO_DEFAULT_SINK=0`, `QL_MIC_DEFAULT_SOURCE=0` | Create the Quest-Link devices without making them the default. |
| `QL_VIDEO_CODEC=h264` | Stream H.264 instead of HEVC. |
| `MONADO_TREE=<path>` | Use another Monado checkout instead of `external/monado`. |

### SteamVR driver (monadolink)

These live in SteamVR's own settings (`steamvr.vrsettings`, section `driver_monadolink`). With SteamVR
**closed**, `scripts/setting.sh` shows them, `scripts/setting.sh KEY VALUE` sets one and
`scripts/setting.sh KEY --unset` restores the default.

| Key | Default | Meaning |
|---|---|---|
| `controllers` | true | Add the Touch controllers. |
| `controller_offset_x/y/z`, `controller_offset_pitch/yaw/roll` | 0, 0, -0.08 m, 22°, 0, 0 | Where SteamVR places the controllers relative to Monado's grip pose (the tuning mode changes these). |
| `controller_tuning` | true | The in-headset tuning mode. |
| `fsr_render_scale`, `fsr_sharpness` | 1, 0.2 | FSR upscaling (the tray's settings win). |
| `overlay_layers` | true | Show SteamVR's overlays (menus, dashboard) composited by monadolink. |
| `overlay_debug` | 0 | 2 = hand overlays to Monado as separate layers instead (works with this Monado). |
| `quiet_logs` | false | Minimal logging (as the tray's Logging setting). |
| `reconnect_interval_ms` | 2000 | How often to retry when Monado isn't there (start it any time). |
| `monado_socket`, `monado_runtime_file`, `openxr_runtime_json` | | Where Monado's socket and OpenXR runtime are (normally found on their own). |

## Troubleshooting

* **SteamVR says "no headset detected"**: Monado isn't running, or Link wasn't accepted in the headset
  in time. Look at `monado-host.log` and SteamVR's `vrserver.txt` (lines starting with
  `monadolink:`). monadolink keeps retrying, so starting Monado afterwards works.
* **The Quest isn't found, or keeps re-pairing**: check that it's on USB 3
  (`cat /sys/bus/usb/devices/*/speed` shows 5000 for it), with another cable or a port on the back
  of the PC.
* **Stutter in busy scenes**: run the tray's USB benchmark and use its bitrate; lower the
  Resolution or the Render resolution if the GPU is the limit.
* **A smear or noise that doesn't go away**: Reset stream (tray, or B in the tuning mode).

## Repository

| Path | What |
|---|---|
| `init.sh` | Fetch the submodules (shallow; `--no-monado` for the driver alone). |
| `build-monado-fedora.sh`, `grant-vr-priority.sh` | Build Monado and give the compositors their GPU priority. |
| `start-vr.sh`, `start-monado.sh`, `vr-tray.py`, `install-vr-launcher.sh` | Starting VR, the tray icon and its menu entry. |
| `steam-env.sh` | Finds Steam (native or Flatpak) for the other scripts. |
| `src/`, `driver/`, `tests/`, `tools/` | monadolink, the SteamVR driver (its design: `DESIGN.md`). |
| `scripts/` | Installing the driver and diagnostics. |
| `external/` | Submodules: Monado, OpenVR, OpenXR SDK, Vulkan headers, doctest, FidelityFX FSR. |

## License

This repository's own code, scripts and documentation are under the **MIT license** (`LICENSE`).
The submodules in `external/` keep their own licenses: see `THIRD_PARTY.md`. In particular the
Monado submodule's Quest Link build is GPL-3.0 (its video encoders come from WiVRn); it is a separate
program that monadolink talks to over IPC, not part of the driver. A built driver carries the
licenses of what is compiled into it (the OpenXR loader, Apache-2.0; FSR, MIT) in its `licenses/`
folder.

## Credits

* The Quest Link driver: Max Thomas' (shinyquagsire23) macOS Quest Link fork of Monado,
  <https://github.com/shinyquagsire23/monado> (branch `macos_ql_merge`), with WiVRn's video encoders
  (Guillaume Meunier, Patrick Nicolas, GPLv3). See `external/monado/README.md`.
* [Monado](https://gitlab.freedesktop.org/monado/monado) (Collabora, Ltd. and contributors).
* AMD FidelityFX FSR 1, Valve's OpenVR headers, Khronos' OpenXR SDK and Vulkan headers, doctest.
