# Licensing

Xendroid+'s own code is Copyright (C) 2026 phforner0 and the Xendroid+
contributors, licensed under the GNU Affero General Public License, version 3 or
(at your option) any later version: `LICENSE` at the repository root.

Original XenDroid/Xenia components and the third-party components below retain
their original licenses. The combined build links a GPL-3.0-or-later LSFG engine
(section 13 of both licenses allows combining it with AGPL-3.0 code); distribution
of that combined binary must provide corresponding source under the applicable
AGPL/GPL terms as well as the original notices. This does not relicense unrelated
upstream BSD/MIT files.

| Component | Pinned source | License and local notice |
|---|---|---|
| Xendroid+ (this project's own code) | This repository | `LICENSE` at the repository root (AGPL-3.0-or-later) |
| Xenia | Existing source tree | `emulator-core/src/main/cpp/xenia/LICENSE` (BSD) |
| xenia-edge (`has207/xenia-edge`) | Upstream ARM64/Vulkan/VFS/XMA2/Kernel commits (`2026-11` series) & `has207/FFmpeg` WMAPro/XMA2 decoder fixes (`3913ff9e`, `9307052d`, `dfc5a399`) | `emulator-core/src/main/cpp/xenia/LICENSE` (BSD-3-Clause; FFmpeg changes under `xenia/third_party/FFmpeg/LICENSE.md`) |
| xendroid-mali (`DEGROUCHO/xendroid-mali`) | GPU compute ASTC 4x4 / EAC R11G11 texture transcoding, Xenos `_EXPAND` half-float conversion, `vulkan_msaa_4x_as_2x2` AFBC EDRAM path, `kMemExportCompute` batching, acyclic SPIR-V segments & shared-memory writewatch release exclusion | `emulator-core/src/main/cpp/xenia/LICENSE` (BSD-3-Clause; original copyright headers preserved) |
| XDroid360 (`TheCafeGuy/XDroid360`) | Static non-finite `OpFMul` analysis, integer-scale texture specialization, persistent `render_targets.vk.bin`, mip-chain direct resolve linking, Dekker scheduler & `WaitMultiple` fixes, `Clock::HoldGuestTime`, AAudio Bluetooth/Loudness Guard & `jit_frame_start` | `emulator-core/src/main/cpp/xenia/LICENSE` (BSD-3-Clause; original copyright headers preserved) |
| XeniOS (`XeniOS` ARM64 backend) | ARM64 LSE `CASAL` leaf thunk reserve fallback (`7e483f44`) | `emulator-core/src/main/cpp/xenia/LICENSE` (BSD-3-Clause) |
| aX360e (`aenu1/ax360e`) | Android SAF (`content://`) multi-file document tree VFS bridge (`saf_tree_device.h` adapted from `document_file`) & Adreno 6xx `bcenabler` integration | WTFPL (`SPDX-License-Identifier: WTFPL`) |
| ZeroFG | Release v1.1.0 (`2b7711e47680c25dd461110c182ee5dd4a637f99`); provenance and mode mapping in `zerofg/SOURCE.md` | `zerofg/LICENSE` (Apache-2.0); Xenia-derived presenter glue retains its BSD notices |
| Mbed TLS (kernel crypto and XEX decryption) | Mbed-TLS/mbedtls `068ff080b369adfac81509f9b57b2afabaf82dc5` (3.6.7; only the crypto sources the build uses) | `xenia/third_party/mbedtls/LICENSE` (Apache-2.0 OR GPL-2.0-or-later) |
| Win-FG | The412Banner/win-fg `fbc69ed6c5bf16b824d0a29bd8b171675101f9dd` | `third_party/winfg/LICENSE` (MIT), FidelityFX notice and THIRD-PARTY.md |
| LSFG host chain | The412Banner/Bannerlator `15d4f73c079824d54c468f079361ce56d88a7c2e` | `third_party/lsfg/LICENSE` and original GPL-3.0-or-later SPDX headers; Eden/WinNative/lsfg-vk credits preserved |
| DXBC translator subset | Same Bannerlator revision, derived from DXVK | `third_party/lsfg-dxbc/LICENSE.md` (zlib-style license) |
| Barlow, Barlow Semi Condensed (app fonts) | google/fonts `ofl/barlow`, `ofl/barlowsemicondensed` (v1.408) | SIL Open Font License 1.1, `app/src/main/assets/font-licenses/Barlow-OFL.txt` |
| JetBrains Mono (app font, IDs and config keys) | google/fonts `ofl/jetbrainsmono` (variable) | SIL Open Font License 1.1, `app/src/main/assets/font-licenses/JetBrainsMono-OFL.txt` |
| Snapdragon Game Super Resolution 1 | SnapdragonStudios/snapdragon-gsr `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e` (`sgsr/v1/include/glsl/sgsr1_shader_mobile.frag`) | `third_party/sgsr/LICENSE` (BSD-3-Clause); ported to XESL as `xenia/src/xenia/ui/shaders/guest_output_sgsr.xesli` with the notice kept |

Paths above are relative to `emulator-core/src/main/cpp` unless fully qualified.
XenDroid-specific modifications are recorded in each `XENDROID_INTEGRATION.md`.
The Win-FG adapter adds bounded allocation/error handling and scratch cleanup;
the LSFG adapter replaces compositor dispatch, bounds cache parsing and provides
a controllable pacing clock for software tests. Original shaders written for
XenDroid's optional SDR filters are generated during the build.

No code from the current CC BY-NC-ND LSFG-VK v2 was imported. Research is in
`docs/lsfg-vk-referencia.md`. `Lossless.dll` and extracted LSFG SPIR-V are
user-provided private runtime data: never included in source, APK, or public
fixtures. Native tests accept a path to the user's DLL and discard the temporary
cache they create.

License texts and engine notices are bundled as offline assets under
`file:///android_asset/engine-licenses/` (fonts: `file:///android_asset/font-licenses/`);
About → Open-source licenses links to them. Distribute the source and build instructions for the exact APK revision,
including these native changes and the imported source/notices.
