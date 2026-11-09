# Third-party components

This repository's own files are under the MIT license (`LICENSE`). The submodules in `external/` keep
their own licenses:

| Component | License | How it's used | License text |
|---|---|---|---|
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (loader) | Apache-2.0 | **Compiled into the driver** (statically linked loader), and its headers | `external/OpenXR-SDK/LICENSE` |
| [FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | MIT | Its shader code is **compiled into the driver** (FSR upscaling) | `external/FidelityFX-FSR/license.txt` |
| [OpenVR](https://github.com/ValveSoftware/openvr) | BSD-3-Clause | Headers; `libopenvr_api.so` for the `monadolink_tune` tool | `external/openvr/LICENSE` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 OR MIT | Headers only | `external/Vulkan-Headers/LICENSE.md` |
| [doctest](https://github.com/doctest/doctest) | MIT | The unit tests only | `external/doctest/LICENSE.txt` |
| Monado with the Quest Link driver (`external/monado`) | BSL-1.0, and **GPL-3.0** for the Quest Link video encoders (from WiVRn) | A separate program, not linked into the driver: monadolink talks to it over IPC | `external/monado/README.md`, each file's header |

A built driver (`build*/monadolink/`, and the copy `scripts/install.sh` puts into SteamVR) carries
`LICENSE`, this file and the license texts of the components compiled into it in its `licenses/`
folder.
