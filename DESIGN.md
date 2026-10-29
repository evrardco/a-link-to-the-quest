# monadolink — a SteamVR direct-mode driver that renders through Monado

Status (2026-10-28): M0 verified; M1 verified on the host (Monado reached from vrserver, live head
poses); **M2 verified on the host 2026-10-29**: SteamVR Home visible in the headset and world-locked
(render pose within ~1° of the tracked pose, 9598/9600 frames shown at 120 Hz); the user noticed a
few late frames (→ M5). 50 unit tests + `monadolink_vk_selftest`.

## Goal

Let SteamVR (and every OpenVR game, including 32-bit ones like HL2:VR) run on top of our
patched Monado + Quest Link stack:

```
OpenVR game ──► SteamVR client ──IPC──► vrserver (64-bit)
                                          │  loads driver_monadolink.so
                                          ▼
                              IVRDriverDirectModeComponent
                              (SteamVR's compositor hands us its layers as DMA-BUFs)
                                          │  Vulkan import + copy
                                          ▼
                              OpenXR client session ──► Monado (monado-service)
                                                         └► compositor ► VAAPI HEVC ► Quest Link ► Quest 2
       poses / buttons / haptics ◄────────────────────────── OpenXR actions & spaces
```

Why this solves the 32-bit problem: the game only ever talks to SteamVR's own client, which
reaches the 64-bit `vrserver` over IPC. The only OpenXR code runs inside our 64-bit driver, so no
32-bit OpenXR loader or 32-bit Monado client is needed.

## Language: C++17

The OpenVR driver API is a set of C++ abstract classes (virtual methods, vtables, `GetComponent`
returning interface pointers). Implementing it from C would mean hand-built vtables matching
MSVC/Itanium ABI details — fragile for no benefit. So the driver is C++, but written in a plain,
C-like style: no exceptions across the OpenVR boundary, no RTTI needs, minimal templates, simple
structs. OpenXR and Vulkan are C APIs and are called directly.

## External libraries

All third-party code lives in **git submodules under `external/<name>`** (shallow clones, pinned):

| Submodule | Pinned at | Why we need it | Licence |
|---|---|---|---|
| `external/openvr` | v2.15.6 | `openvr_driver.h`: the driver interfaces SteamVR loads (`IServerTrackedDeviceProvider_004`, `ITrackedDeviceServerDriver_005`, `IVRDisplayComponent_003`, `IVRDriverDirectModeComponent_009`, `IVRIPCResourceManagerClient_003`, `IVRDriverInput_004`). Header only; SteamVR provides the implementations at runtime. Also has Valve's sample drivers (`samples/drivers/drivers/simplehmd`). | BSD-3 |
| `external/OpenXR-SDK` | release-1.1.65 | M1+: headers, and the loader built **statically** into the driver so nothing extra is needed inside the Steam Linux Runtime. Finds Monado through `active_runtime.json` / `XR_RUNTIME_JSON`. | Apache-2.0 |
| `external/Vulkan-Headers` | main (shallow) | M2+: headers for importing SteamVR's DMA-BUF textures (`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_semaphore_fd`, timeline semaphores). The Vulkan *loader* stays the system `libvulkan` (it must match the installed GPU drivers). | Apache-2.0 / MIT |
| `external/libdrm` | main (shallow) | M2+: `include/drm/drm_fourcc.h` (DRM fourcc and modifier constants for `DmabufAttributes_t`). | MIT |
| `external/doctest` | v2.5.0 | Unit tests (single header). | MIT |

Build-time only, later: **glslang** if we ever composite layers in a compute shader.

Deliberately *not* used:

- **Monado's source / internal IPC**: going through OpenXR keeps the driver independent of
  Monado's unstable internals and works with any OpenXR runtime (WiVRn too).
- **A math library**: we only need pose/quaternion/3x4-matrix conversions between OpenXR and
  OpenVR conventions; a small header is enough.
- **A JSON library**: the manifest, settings and input profiles are static files.

## Repository layout

`[x]` = exists (M0), `[ ]` = planned.

```
monadolink/                    its own git repo (inside the oculus-quest-2 project folder)
├── DESIGN.md                  this file
├── CMakeLists.txt             [x] option MONADOLINK_BUILD_TESTS (ON by default)
├── external/                  [x] git submodules: openvr, OpenXR-SDK, Vulkan-Headers, libdrm, doctest
├── driver/                    [x] copied as-is into build/monadolink (= the SteamVR driver folder)
│   ├── driver.vrdrivermanifest
│   └── resources/
│       ├── driver.vrresources
│       ├── settings/default.vrsettings      serial/model, log_frames, render size, refresh rate
│       └── input/
│           ├── monadolink_hmd_profile.json  [x]
│           (controllers use SteamVR's own {oculus}/input/touch_profile.json, setting controller_input_profile)
├── src/
│   ├── driver_main.cpp        [x] HmdDriverFactory() export
│   ├── device_provider.*      [x] IServerTrackedDeviceProvider: connect to Monado, add devices, tracking
│   │                              thread (500 Hz head poses), RunFrame, drain events
│   ├── display_config.h       [x] DisplayConfig: settings defaults, overridden by Monado
│   ├── hmd_device.*           [x] ITrackedDeviceServerDriver + DisplayComponent; latest pose (thread-safe)
│   ├── controller_device.*    [x] M4 Touch controllers: "oculus_touch", Quest 2 render models, input
│   │                              components (changes only), haptic component
│   ├── direct/direct_mode.*   [x] IVRDriverDirectModeComponent: swap sets via NewSharedVulkanImage,
│   │                              index rotation, layer/present logging with pid/tid, placeholder pacing
│   ├── direct/{layer_queue,sync}.*   [ ] M2/M3
│   ├── xr/xr_client.*         [x] M1 OpenXR instance + headless session (XR_MND_headless), display info,
│   │                              head pose at CLOCK_MONOTONIC "now" (XR_KHR_convert_timespec_time)
│   ├── xr/xr_math.h           [x] M1 pose/velocity, FOV tangents, eye-to-head matrix conversions
│   │                              M2: Vulkan session (XR_KHR_vulkan_enable2) + one swapchain per eye,
│   │                              frame API (wait/begin, acquire/release, end with a projection layer)
│   ├── xr/xr_input.*          [x] M4 action set for /interaction_profiles/oculus/touch_controller (hands
│   │                              as subaction paths), grip spaces, haptics; retries bindings without
│   │                              thumbrest/menu if refused
│   ├── vk/vk_context.*        [x] M2 dlopen'ed libvulkan.so.1 + function table; instance/device from OpenXR
│   ├── vk/vk_copy.*           [x] M2 VkCopier: import SteamVR's opaque fds, wait on DMA-BUF write fences
│   │                              (sync file), blit bounds -> swapchain image, synchronous
│   └── util/log.h             [x] VRDriverLog wrapper + process_tag() (pid/comm/tid)
│       util/timing_stats.h    [x] per-interval frame timing line ("timing N s: ..."); interval =
│                                  ML_TIMING_STATS_INTERVAL_S (CMake MONADOLINK_TIMING_STATS_S, default 1, 0 = off)
│       util/stderr_capture.h  [x] forwards stderr to the SteamVR log while connecting (the loader's and
│                                  Monado's error messages)
├── tests/                     [x] doctest; fake_openvr.h fakes every SteamVR host interface we use
│   ├── test_factory.cpp, test_device_provider.cpp, test_display.cpp, test_direct_mode.cpp
│   ├── test_xr_math.cpp, test_xr_client.cpp
│   └── fake_openxr_runtime.cpp  a minimal OpenXR runtime (+ generated build/fake_openxr_runtime.json)
│                              loaded by the real static loader; state in fake_openxr_state.h
├── tools/xr_probe.cpp         [x] monadolink_xr_probe: XrClient against the active runtime, no SteamVR
├── src/offset_tuner.h         [x] in-headset offset tuning with the controllers (pure logic)
├── tools/tune.cpp             [x] monadolink_tune: live controller offset tuning through SteamVR's settings
├── tools/vk_selftest.cpp      [x] monadolink_vk_selftest: the copy path on the real GPU (readback checks)
└── scripts/install.sh         [x] build + copy into <SteamVR>/drivers/monadolink (Flatpak or native)
```

The object library `monadolink_core` holds all driver code; both `driver_monadolink.so` and
`monadolink_tests` link it, so tests run exactly the code SteamVR loads. The `.so` exports only
`HmdDriverFactory` and links libstdc++/libgcc statically when the static library exists (Fedora:
`libstdc++-static`), otherwise dynamically. Host builds use `build-host/`, sandbox builds `build/`.

## Data flow per frame

1. **Pacing.** The OpenXR thread calls `xrWaitFrame` → predicted display time `T` from Monado
   (which follows the headset's 120 Hz). SteamVR is paced to it by blocking in `Present`/`PostPresent`
   only as long as needed (ALVR learned that holding SteamVR's submit thread for a whole frame hurts).
2. **Poses.** We report head and controller poses for `T` with `TrackedDevicePoseUpdated`, located
   with `xrLocateSpace` (VIEW space for the head, action spaces for the controllers), converted to
   SteamVR's time base.
3. **SteamVR renders** the game, composites its own overlays, and calls `SubmitLayer` once per layer
   (eye textures + `bounds` + `mHmdPose` used to render) then `Present`.
4. **Sync.** We wait until SteamVR's GPU work on those textures is done (see open questions).
5. **Copy.** For each layer: import the texture once (cached per shared handle), copy the `bounds`
   sub-rectangle into an acquired OpenXR swapchain image.
6. **Submit.** `xrEndFrame` with one `XrCompositionLayerProjection` per SteamVR layer, each carrying
   the pose SteamVR rendered with — so Monado's compositor does the timewarp/reprojection, and blends
   overlay layers (premultiplied alpha) instead of us writing a compositor.
7. Monado encodes and streams as today.

## Milestones

| # | Result | Proves |
|---|---|---|
| M0 | Driver loads; a "Monado Link" HMD appears in SteamVR (static pose, no display); direct-mode calls logged with pid/tid — **verified 2026-10-28: the HMD appears in SteamVR (host, RPM Steam, SteamVR 2.18.4)**; direct-mode path confirmed in `vrserver` (see open questions) | manifest, factory, properties, which process calls direct mode |
| M1 | Head tracking from Monado moves SteamVR's view — **implemented 2026-10-28, host test pending** | OpenXR session inside vrserver, time conversion |
| M2 | SteamVR Home visible in the headset (single layer, one copy) — **verified 2026-10-29** | direct mode, DMA-BUF import, OpenXR submit |
| M3 | Dashboard over games, correct sub-rectangles, no right-eye stutter | multi-layer, bounds, sync |
| M4 | Touch controllers: buttons, sticks, poses, haptics — **verified 2026-10-29** (SteamVR controller test, HL2:VR played; offset tuning pending) | OpenXR actions ⇄ IVRDriverInput |
| M5 | Pacing/latency tuning at 120 Hz — the user still feels some latency (2026-10-29). Candidates: VAAPI async_depth 2 (+1 frame; retest 1 now that the keyframe stalls are gone), the OpenXR frame blocking SteamVR's submit thread (~7.5 ms in xrWaitFrame; SteamVR predicts ~22 ms ahead), double prediction (SteamVR + Monado ~10 ms). Measure with the timing line first. Step 1: early wait (see M5 decisions). | GetFrameTiming, PostPresent throttling |
| M6 | `install.sh`, Flatpak Steam notes, HL2:VR test | packaging, the 32-bit win |

## M1 decisions

- **Headless session** (`XR_MND_headless`, which Monado's client supports): no graphics binding, so no
  Vulkan and no GPU work in M1. Monado doesn't send state events to headless sessions beyond the
  initial READY (we begin it) — space location works regardless. M2 replaces it with a Vulkan session.
- **Poses at "now"**, not at a predicted display time: the tracking thread locates VIEW in the base
  space at the current CLOCK_MONOTONIC time every 2 ms and reports it with velocities and
  `poseTimeOffset = 0`; SteamVR does its own prediction. (M5 may revisit.)
- **Base space STAGE** (Monado's tracking root, floor level for the Quest) = SteamVR's standing
  universe; fallback LOCAL lifted by 1.6 m.
- **Display from Monado** at Init, before the HMD is added: render size =
  `recommendedImageRect` (larger eye), FOV from `xrLocateViews` in VIEW space, eye-to-head poses →
  `SetDisplayEyeToHead` + `Prop_UserIpdMeters`, refresh from `xrGetDisplayRefreshRateFB`.
- **Without Monado** at Init: settings defaults and the M0 static pose (SteamVR still runs). If Monado
  goes away later, the HMD is reported disconnected; **no reconnect yet** (restart SteamVR).
- Setting `driver_monadolink/openxr_runtime_json`: exported as `XR_RUNTIME_JSON` before the loader
  runs. **Required in practice**: inside vrserver the default runtime is SteamVR's own (see
  Containers below); `install.sh` fills in this project's Monado manifest.
- The loader's and Monado's stderr is forwarded to vrserver.txt while connecting ("OpenXR stderr:").
- Loader quirk: after `xrEnumerateInstanceExtensionProperties` the loader keeps the runtime (and its
  extension list) loaded until an instance is created and destroyed.

## M2 decisions

- **SteamVR allocates, we import** (like ALVR): `NewSharedVulkanImage` with extra flags
  `MUTABLE_FORMAT` and usage `TRANSFER_SRC|SAMPLED|INPUT_ATTACHMENT` (`k_shared_image_*` in
  vk_copy.h), then `RefResource` + `ReceiveSharedFd` → an opaque Vulkan fd, imported with the same
  create parameters (dedicated allocation). The extra reference is dropped with the set.
- **Layouts**: an imported texture leaves UNDEFINED once (→ GENERAL) and then stays in GENERAL; a
  per-frame UNDEFINED transition could discard (or with DCC reinitialise) SteamVR's rendering. The
  self-test checks that a second frame arrives.
- **Sync**: the copy waits on the DMA-BUF's write fences (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` → temporary
  sync-fd semaphore import); works on the opaque fds RADV hands out. ALVR found that SteamVR's own
  scene writes sometimes carry only placeholder fences (it adds a 4 ms guard sleep) — watch for
  tearing/flicker in M2/M3. The copy itself is synchronous (fence wait, 0.5 s cap), so SteamVR can
  reuse the texture as soon as Present returns.
- **Pacing**: Present runs the whole OpenXR frame on SteamVR's submit thread; `xrWaitFrame` paces
  SteamVR (PostPresent no longer sleeps then). Simple, maybe a frame of latency: M5.
- **Scaling**: SteamVR renders at its own supersampled size (2260×2364); the blit scales (linear)
  into Monado-sized swapchains (1926×2015).
- **Poses**: the layer gets SteamVR's render pose (`mHmdPose`) ∘ Monado's eye-in-head poses, in the
  base space; Monado's compositor reprojects to the latest pose.
- **Only the first layer** is shown (M3: all layers).
- Diagnostics in vrserver.txt on logged frames (first `log_frames`, then every 1200): RGBA of SteamVR's
  left texture at its centre and quarter points, read in the copy's own submit — tells a flat image
  rendered by SteamVR from one produced by our copy. First host run (2026-10-28): 1199/1200 frames
  shown at 120 Hz, the user saw a uniform-colour void; quitting Monado first → INSTANCE_LOST handled,
  HMD reported disconnected, SteamVR kept running.
- **Pose guard** (after the 2026-10-28 SteamVR Home test, where the scene showed but didn't follow the
  head and made the user sick): each frame, the rotation between SteamVR's render pose (`mHmdPose`)
  and the latest tracked head pose must be ≤ 20°, else the frame is dropped (black) instead of
  shown. Both poses are logged every 120 frames ("render pose ... | tracked ... | differ by").
  **Cause found:** every pose was reported as `TrackingResult_Fallback_RotationOnly` (300) because
  Monado sets POSITION_VALID but not POSITION_TRACKED for the Quest; SteamVR then stops rendering
  with the HMD's poses (view stuck to the face). Valid position → `Running_OK` fixed it. The guard
  later caught one real glitch: Monado briefly reporting its default pose (1.6 m, identity).
- **Projection convention** (fixed 2026-10-29, after "the FOV feels odd in SteamVR Home"): OpenVR's
  raw projection wants `top = tan(down edge)` (negative) and `bottom = tan(up edge)`; M0–M2 had them
  swapped (copied from Monado's old steamvr plugin), so SteamVR rendered the Quest's 48° up / 50° down
  eye as 50°/48° while Monado displayed it as 48°/50°. A test now compares SteamVR's
  ComposeProjection matrix from our values with OpenXR's for the same FOV. Note Monado's Quest 2 FOV
  itself is a guessed IPD table (outer 52°/49°/43° at 58/65/68 mm, inner 45°, up 48°, down 50°).
- Test-fake lesson: fakes of SteamVR interfaces that the driver calls from several threads (the
  logger, the host) must be thread-safe like the real ones; an unlocked vector caused rare crashes.
- `submit_frames` setting (default true): false = M1 behaviour (headless session), the way out if
  the Vulkan path misbehaves. `libvulkan.so.1` is dlopen'ed (no build dependency).

## M4 decisions

- **Present as Oculus Touch** (like ALVR's Quest 2 emulation): `Prop_ControllerType_String =
  "oculus_touch"`, SteamVR's `{oculus}/input/touch_profile.json`, render models
  `oculus_quest2_controller_left/right`, so games' Oculus Touch bindings apply. Input paths as ALVR
  creates them: trigger/grip value+click+touch, joystick x/y/click/touch, thumbrest/touch, A/B (right),
  X/Y + menu as `/input/system/click` (left), `/output/haptic`. Trigger/grip click = value > 0.7.
- **One OpenXR action set**, hands as subaction paths ("lower button" = A right / X left); actions are
  synced on the tracking thread with the head (500 Hz); SteamVR only gets components that changed.
  Input needs a FOCUSED session (Monado focuses our Vulkan session once it shows frames).
- **Controller pose** = OpenXR grip pose ∘ offset (settings `controller_offset_x/y/z` in m,
  `_pitch/_yaw/_roll` in degrees; the right hand mirrors the left). Default (since 2026-10-29): pitch 22°,
  z −0.08 m, tuned by eye in HL2:VR with the in-headset tuner (ALVR's SteamVR offset was (0, 0, −0.11)). Monado's Quest Link driver reports grip = aim = the raw controller pose tilted 35°,
  so the default will probably need tuning by eye. First HL2:VR session: the gun aimed slightly
  low → a positive `controller_offset_pitch` (tilts forward up). xrizer's Touch offset (grip → raw:
  inverse of Rx(20.6°) with (±0.007, −0.0018, 0.102)) assumes a spec grip pose, which Monado's isn't.
  `scripts/setting.sh KEY VALUE` writes user overrides into steamvr.vrsettings (SteamVR closed).
- **Live offset tuning**: RunFrame re-reads the six `controller_offset_*` settings every 0.5 s;
  `tools/tune.cpp` (`monadolink_tune`, an OpenVR background client linked against the submodule's
  libopenvr_api.so) changes them through IVRSettings while SteamVR runs (w/s pitch, a/d yaw, q/e roll,
  i/k forward/back, j/l sideways, u/o up/down, 0 defaults); SteamVR persists them on exit.
- **Install by symlink** (`scripts/install.sh --link`, once, on the host): SteamVR's
  `drivers/monadolink` points at the sandbox's `build/monadolink`, so a sandbox rebuild is the
  install. The sandbox (Debian 13) build needs glibc ≤ 2.38 and links libstdc++ statically, so it
  loads in vrserver like the host build. Monado's manifest path is baked into the build's
  `default.vrsettings` by CMake (`MONADOLINK_OPENXR_RUNTIME_JSON`) instead of install.sh's sed.
- **In-headset offset tuning** (`src/offset_tuner.h`, setting `controller_tuning`, default on): hold
  both thumbsticks clicked 1 s to enter/leave (long buzz / short buzz). While tuning, the game gets
  no buttons or sticks (poses still flow, so the gun visibly moves); one step per stick flick, a
  haptic tick each: right stick = pitch/yaw (1°), + right grip = height (5 mm)/roll, left stick =
  forward-back/sideways (5 mm), A = defaults. Directions are as seen from the right hand (the left
  mirrors). The tracking thread applies steps at once; RunFrame writes them with
  IVRSettings::SetFloat (A: RemoveKeyInSection), and the 0.5 s reload skips while unsaved steps
  are pending so it can't undo them.
- Haptics: `VREvent_Input_HapticVibration` (RunFrame) → `xrApplyHapticFeedback` on that hand.

## M5 decisions

- **Baseline** (2026-10-29 HL2:VR session, timing lines): SteamVR's Present spent ~7 ms blocked in
  `xrWaitFrame`, then Monado's display time was ~10 ms ahead; SteamVR rendered poses predicted ~22 ms
  ahead of SubmitLayer. The Quest corrects head *rotation* itself (timewarp with the stream pose), so
  the felt latency is mostly what prediction must cover: controllers, head position, game response.
- **Early wait** (setting `early_wait`, **default off** since the second host run, see below): PostPresent runs `xrWaitFrame` for the next
  frame, so SteamVR renders inside Monado's window and Present only begins, copies and ends the
  frame. The waited frame is used by the next Present that has layers; it's dropped if the session
  stops meanwhile. `false` restores waiting in Present.
- **IPD**: Monado can report 0 (both eyes at one point: no stereo, hands huge up close). An IPD
  outside 50-80 mm is ignored: 64 mm default eyes until a real one comes. The tracking thread
  re-locates the views every second; a change goes to SteamVR (`SetDisplayEyeToHead`,
  `Prop_UserIpdMeters`) and into the layer eye poses. The projection isn't updated live (Monado's FOV
  depends on its IPD table too).
- **Timing line metrics** for this: `predict` (SteamVR's prediction from SubmitLayer), `margin`
  (Monado's display time minus our xrEndFrame: slack left before Monado needs the frame) and
  `mismatch` (SteamVR's predicted display time minus Monado's). Early wait should turn ~7 ms of
  `wait` inside `frame` into a smaller `margin`; if `margin` gets near 0, frames miss Monado's slot.
- **First host run of early wait** (2026-10-29, together with async_depth 1): SteamVR side healthy
  (120/s, `frame` ~1 ms in Present). The wait moved to PostPresent and SteamVR's next Present comes
  ~0.5 ms after it, so frames reach Monado ~7 ms sooner after SteamVR rendered them: `mismatch` went
  from ~5 ms (wait in Present: 7 + 10 vs 21.7 predicted) to ~11.5 ms. The double images came from
  async_depth 1 (Monado dropping frames), not from early wait; to be confirmed with depth 2.
- **Early wait collapses under load** (second host run, depth 2, HL2:VR): SteamVR calls Present
  before the game's GPU work is done, so our copy waits for its fences (`copy` 0.3 → 5 ms). Monado's
  app pacer counts wait→end as app time, wakes us earlier (`ahead` 10 → 16 ms), and within seconds
  the loop locked at 30 fps (`copy` 18 ms, `ahead` 30 ms): repeated frames, double images. SteamVR's
  compositor is pipelined: we can't make it render inside Monado's window. Waiting in Present keeps
  Monado's view of our frame time honest (~0.5 ms). Kept as an option, off.
- **async_depth 1 works** once early wait is off (third host run, 8 min of HL2:VR, "really great"):
  now the start-monado.sh default. Remaining drops: game hitches (SteamVR gaps of 0.25-1.7 s, level
  loads), ~17 encoder stalls >12 ms in 8 min (one frame each), and 0.5-2 MB frames after scene
  changes (bounded in Monado with a minimum QP of 20).
- **No pose prediction anywhere** (found 2026-10-29 after "controllers look smoother than the world
  when my head moves"): Monado's Quest Link driver passed the headset's velocities without the
  VELOCITY_VALID flags, so Monado predicted nothing and monadolink sent SteamVR zero velocities.
  Monado's `QL_POSE_STATS` showed the velocities are correct (base frame, scale 1.00, poses at
  ~535 Hz, 3.4 ms old); fixed in Monado (`QL_PREDICT`, 50 ms cap). monadolink needed no change: it
  forwards velocities whenever they're valid, and SteamVR now extrapolates its ~22 ms.
  **Verified on the host 2026-10-29: "it feels perfect now"** (162 s: render vs tracked pose 0-2°,
  2 s under 115 fps). This, with async_depth 1, closes the latency part of M5.
- Older note — next candidates were: VAAPI async_depth 1 (`QL_VAAPI_ASYNC_DEPTH=1`, −1 frame; its periodic stalls may
  be gone with the keyframes), and matching SteamVR's prediction to the real photon time (vsync to
  photons / pose time offsets) once the timing line shows the mismatch.

## Open questions / risks

- ~~Which process calls the direct-mode methods, and on which thread?~~ **Answered by M0 (2026-10-28,
  SteamVR 2.18.4):** all of them run in **`vrserver`** (our process). `CreateSwapTextureSet` comes from
  one thread; every `SubmitLayer`/`Present` from a single dedicated submit thread. The textures belong to
  the compositor process (another pid) and reach us as shared handles. Observed per frame: 2 swap sets
  (one per eye) × 3 textures, **VkFormat 43 (`R8G8B8A8_SRGB`), 2264×2372** (our 1832×1920 × SteamVR's auto
  render scale 1.236), **1 layer**, full bounds, no depth, `syncTexture` = 0, pose predicted ~21.8 ms
  ahead. Placeholder pacing measured 119.6 fps. ⇒ M2 must scale (blit) into Monado-sized swapchains, and
  sync without a sync texture (implicit fences on AMD; timeline semaphores like ALVR otherwise).
- **Synchronisation.** AMD DMA-BUFs carry implicit fences; NVIDIA's don't. ALVR waits on SteamVR's
  per-texture timeline semaphores through an *undocumented* shared-memory state reached via
  `IVRIPCResourceManagerClient`. We're on AMD, so implicit sync may suffice at first; plan for the
  semaphore path.
- **Double compositing.** SteamVR composites, then Monado composites again: roughly one extra GPU
  pass and possibly a frame of latency. Acceptable for a first version; a later optimisation could
  hand SteamVR's final image straight to Monado as a single layer with no reprojection.
- **Containers / runtime choice.** *Seen in the M1 host test (native Steam, SteamVR 2.18.4):* vrserver
  runs inside the Steam Linux Runtime container, and there **the default OpenXR runtime is SteamVR
  itself**: our xrCreateInstance connected back to vrserver (log: "New Connect message from vrserver
  (VRApplication_OpenXRInstance)"), which was blocked in our Init → XR_ERROR_RUNTIME_FAILURE after
  20 s. Fix: `install.sh` writes Monado's manifest into `openxr_runtime_json`, and XrClient refuses a
  SteamVR manifest (steamxr / vrclient.so) at once. Monado's socket was visible with the launch option
  `PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc %command%` (not yet known whether
  it's needed without it); the driver logs both the manifest and the socket. SteamVR runs inside the Steam Linux Runtime (and Flatpak, here). The driver
  `.so`, the OpenXR loader, Monado's client library and Monado's socket must all be reachable from
  there — the same plumbing we already did for xrizer.
- **Same GPU.** Our Vulkan device must be the GPU Monado picks (`xrGetVulkanGraphicsDevice2`) and the
  one SteamVR renders on; on this machine there is only the RX 6700 XT, but we should check.
- **Fresh API.** Linux direct mode exists since SteamVR 2.5 but ALVR only switched to it on
  2026-10-04 and is still fixing issues (AMD/VAAPI bitrate, NVIDIA texture creation, Intel black
  screen). Expect to hit SteamVR-side bugs.

## References

- OpenVR driver header: `IVRDriverDirectModeComponent_009`, `IVRIPCResourceManagerClient_003`
  (github.com/ValveSoftware/openvr, headers/openvr_driver.h)
- SteamVR 2.5: "Add support for direct mode drivers (IVRDriverDirectModeComponent) on Linux."
- ALVR PR #3398 (Linux direct mode) and #3425 (frame sync, layers, bounds, AMD DCC ownership)
- Monado `src/xrt/state_trackers/steamvr_drv` — the existing plugin (devices only, no frames)
