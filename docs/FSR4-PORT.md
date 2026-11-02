# FSR 4 upscaler (2026-10-26)

AMD's machine-learning upscaler (FSR 4, provider "FSR 4.1.2") as an alternative to FSR 3.1 in the Vulkan renderer
(`docs/FSR-PORT.md` describes FSR 3.1 and everything around it, which this builds on). It runs **natively on Vulkan**,
on the game's own VkDevice and command buffer: no Direct3D 12 bridge, no shared images, no split submits. It takes
exactly the inputs fsr.c prepares for FSR 3.1 (colour, depth, camera + object motion vectors, reactive mask, jitter,
camera near / far / FOV) and is dispatched where the FSR 3.1 dispatch is. Default stays FSR 3.1, FSR stays Off by default.

Tested on: RX 9070 XT, Adrenalin 26.10.1 (Vulkan driver 1.4.359), Windows 11. FSR 4 needs an RDNA 4 GPU and the
driver package that contains `amdvkffx64.dll` (v2.4.0.4370 here).

## Settings

    [Upscaler]
    FSR = On
    Upscaler = FSR4        ; FSR3 (default) or FSR4
    FSRQuality = Quality   ; same ratios as FSR 3.1: NativeAA 1x, Quality 1.5x, Balanced 1.7x, Performance 2x, UltraPerformance 3x
    Sharpness = 0.8        ; the provider's RCAS sharpening (same key as FSR 3.1)

An existing mhgurecomp.ini has no `Upscaler` key (only whole missing sections are added): add the line by hand.
Test overrides: `MHGU_FSR_UPSCALER=3|4`, `MHGU_FSR4_DLL=<path>` (another DLL; a missing path shows the fallback),
`MHGU_FSR4_LINEAR=1` (colour not flagged as gamma-encoded, A/B), `MHGU_FSR4_FAILTEST=n` (the n-th FSR 4 dispatch is
reported as failed, to see the fallback), `MHGU_FSR_DEBUG=1` (FFX input checks), `MHGU_FSR_LOG=1` (per second:
`upscaler FSR 4.1.2, FSR 4 frames N`).

To try it: set `FSR = On`, `Upscaler = FSR4` (any FSRQuality) and start the game; log.txt has the `[fsr4]` lines.

## What is included

FSR 4 on Vulkan is part of this port and is **opt-in**: it needs `FSR = On` and `Upscaler = FSR4` (default `FSR3`; FSR itself is Off by
default), and if it cannot start the game uses FSR 3.1 instead (every reason is logged with a `[fsr4]` line). No AMD file is in this
repository or in a release.

Routes, tried in this order (each logs which one was used; `MHGU_FSR4_ROUTE=driver|loader` tries just one, for testing):
1. **the driver's provider**: the game loads `amdvkffx64.dll` from the player's AMD driver and asks it for the FSR 4 provider. Log:
   `SDK-module check hook installed`, `route: the driver's provider`.
2. **a loader DLL beside the exe** (experimental, see below): used through the public ffx-api (`ffxQuery` to find a 4.x version, then
   `ffxCreateContext` / `ffxDispatch`). Log: `loader ...`, `route: the player-supplied loader`.
3. **FSR 3.1.**

## Getting the FSR 4 DLL

No AMD file is in this repository or in a release. On a Radeon with FSR 4 support (RDNA 4) and a recent Adrenalin driver there is nothing to
download: the driver installs `amdvkffx64.dll` itself and the game finds it automatically: beside `mhgurecomp.exe` if you put a copy there,
else in the driver-store folder of your GPU, else the newest copy under
`C:\Windows\System32\DriverStore\FileRepository\u*.inf_amd64_*\B*\amdvkffx64.dll`. `MHGU_FSR4_DLL=<path>` uses a different file.

The driver only hands out its provider when it sees the FidelityFX loader modules in the process (it asks with `GetModuleHandleW`), so the game,
in its own memory only (nothing on disk), points that one import of the driver DLL at a function that answers "loaded" for an `amd_fidelityfx_*`
module and defers to the real function for everything else (`patch_import` in `fsr4.c`; the idea comes from reading OptiScaler's source, no code
copied). A driver update that changes that check could stop it working; then FSR 3.1 is used.

*Experimental, route 2:* the code also accepts a loader DLL named `amd_fidelityfx_vk.dll` placed beside `mhgurecomp.exe` (or
`MHGU_FSR4_LOADER=<path>`), used through the public ffx-api without any patching, if one exists that offers FSR 4. The `amd_fidelityfx_vk.dll`
v1.0.1 found on the test machine does not (it offers FSR 3.1.4 and 2.3.3 only, so the game logged `this loader offers no FSR 4` and ran FSR 3.1).
This route is implemented but unverified; its backend descriptor id (0x3, from ffx-api's public Vulkan header) is an assumption.

Notes: the vendored `third_party/ffx-api` headers and the FSR 3.1 sources in `third_party/fsr-vulkan` are MIT per AMD's licence file in the
FidelityFX SDK (texts kept and listed in `THIRD_PARTY_NOTICES`). The in-memory import step changes AMD's driver DLL in this process; the author
has not reviewed it against AMD's driver terms. For that reason the patch is **off by default**: a normal `make` compiles none of it and FSR 4 is then
only available if a loader as described above exists, otherwise FSR 3.1 runs. `make FSR4_DRIVER_PATCH=1` builds the driver route in (this is what the
measurements below were made with); it is a build switch, so a distribution can decide, or players can build it themselves.

## How it works (runtime/nvn/vulkan/fsr4.c, fsr4.h)

- The AMD Vulkan driver package carries `amdvkffx64.dll`. It exports `UpdateFfxApiProvider` / `UpdateFfxApiProviderEx`,
  the FidelityFX "external provider" hook (the same one the D3D12 side uses with `amdxcffx64.dll`; the MIT ffx-api
  source of SDK 1.1.4, `ffx_provider.cpp`, shows the D3D12 caller). Called with a description type it fills in a table of
  function pointers (create / destroy / configure / query / dispatch, plus `isSupported`), for the upscaler: FSR 4.
- (Route 1 only, `FSR4_DRIVER_PATCH=1`.) Found at start-up: beside the exe, else in the driver store folder of the GPU (D3DKMT user-mode driver path of the
  Vulkan device's LUID), else the newest copy in `System32\DriverStore\FileRepository`. Loaded with LoadLibrary at
  run time; **nothing of AMD's is built in or shipped** (`*.dll` is git-ignored).
- The driver only hands out providers when it sees FidelityFX loader modules loaded: it asks
  `GetModuleHandleW(L"amd_fidelityfx_loader_vk.dll")` / `..._upscaler_vk.dll`. Without them the call fails with
  E_INVALIDARG (checked). fsr4.c patches that one import of the driver DLL so it gets the DLL itself as the answer.
  This is how OptiScaler (GPL-3.0; its source was read as a reference for how the driver is reached and which
  descriptors it takes - no code copied) does it; the code here is fresh.
- Context: `ffxCreateContext` through the provider with `ffxCreateContextDescUpscale` (flag
  `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE`) -> the Vulkan backend descriptor (SDK 2.x type id `0x01000003`, same layout
  as `ffxCreateBackendVKDesc`: device, physical device, vkGetDeviceProcAddr) -> a version descriptor (`0x0001000B`,
  version 4.1.2; the driver accepts 4.0.0 .. 4.1.2). Created once, at the first FSR frame (66-73 ms), because the
  ratio does not change during a run.
- Dispatch: `ffxDispatchDescUpscale` (the ffx-api 1.1.4 layout works unchanged) with VkImage resources and the
  command buffer; all images are in GENERAL (the renderer's rule) = state "unordered access" for every resource. Colour
  is the game's gamma-encoded LDR RGBA8, so `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB` is set (depth: standard Z, no
  flags; motion vectors in render pixels, jitter removed; motionVectorScale 1; preExposure 1).
- Jitter: the same picture shift FSR 3.1 gets (the camera hook applies AMD's Halton jitter, 8 (output/render)^2 phases).
- fsr.c: `run_fsr` is shared; after the prep / object-velocity / finish passes it records either the FSR 3.1 dispatch
  or `vk_fsr4_dispatch`. On any FSR 4 failure it logs why, switches FSR 4 off for the run and records the FSR 3.1
  dispatch for that same frame with a history reset (FSR 3.1's context is created only then).
- Device (device.c, only with FSR on and `Upscaler = FSR4`): extensions `VK_KHR_cooperative_matrix`,
  `VK_EXT_shader_float8`, `VK_KHR_synchronization2`, `VK_EXT_subgroup_size_control` and the features the provider's
  shaders declare (cooperative matrix, float8 + float8 cooperative matrix, synchronization2, subgroup size control /
  full subgroups, shaderFloat16 / Int8 / Int16, buffer device address, Vulkan memory model, 8 / 16-bit storage,
  variable pointers, descriptor indexing, storage image read / write without format); instance apiVersion 1.3 instead
  of 1.2 (its SPIR-V is 1.6). If the GPU lacks any of it: logged, FSR 3.1 runs. With FSR off or `FSR3` nothing changes.

Log lines (log.txt / stdout): `[fsr4] loaded <path> (<where>), file version`, `SDK-module check hook installed`,
`driver provider "FSR 4.1.2" id 0x...`, `the provider accepts this device`, `FSR 4.1.2 context WxH -> WxH created in N ms
(...formats...)`, `first FSR 4.1.2 dispatch recorded`, `[fsr] on (FSR 4.1.2): ...`, failures: `[fsr4] ... FSR 3.1 is used` /
`... FSR 4 is off from here on, FSR 3.1 takes over`.

## Files

New: `runtime/nvn/vulkan/fsr4.c`, `fsr4.h`; `third_party/ffx-api/` (ffx_api.h, ffx_api_types.h, ffx_upscale.h from the
FidelityFX SDK v1.1.4, MIT, licence file kept); this file.
Changed: `runtime/config.c/.h` (`Upscaler` key, `MHGU_FSR_UPSCALER`), `runtime/nvn/vulkan/device.c` (the FSR 4 extensions,
features, apiVersion), `runtime/nvn/vulkan/fsr.c` (the branch in `run_fsr`, init, a log line), `Makefile`
(`-Ithird_party/ffx-api`, `-lversion`, `FSR4_DRIVER_PATCH`, default 0). Not changed: shaders, textures, exec.c, the camera / jitter code.

Note: the conventions used here (4.x provider chosen by version id, gamma-encoded LDR colour with the non-linear flags,
depth / motion-vector conventions, one jitter convention for all providers) were worked out with a Direct3D 12 route
first; the Vulkan provider made that route unnecessary, so it is not part of this port.

## Measured (RX 9070 XT, 1920x1080 window, quest 101, hunter standing, 4 frames per shot; MHGU_CONTROL gpuprof)

FSR 4 ran every frame: ~60 `FSR 4 frames` a second in every one-second log line while the quest scene was shown
(2170 frames in the Quality run, 2211 in the NativeAA run, the rest being the town / loading frames, scaled as before).

GPU time (prep + FSR stage = the gap before the HUD pass; whole frame), one sample each:

| setting | FSR 3.1 stage / frame | FSR 4 stage / frame |
|---|---|---|
| Quality (1280x720 -> 1080p) | 0.886 / 1.499 ms | 1.301 / 1.915 ms (colour unflagged: 1.102 / 1.714; Sharpness 0: 1.215 / 1.831) |
| NativeAA (1080p) | 1.488 / 2.414 ms | 1.640 / 2.492 ms |

So FSR 4 costs ~0.4 ms more than FSR 3.1 at Quality and ~0.15 ms more at NativeAA here; 60 fps (vsync) in all runs.

Image metrics (`metrics.py`: PSNR dB against the 4K supersampled picture / sharpness relative to it / flicker x1000 over
4 static frames; regions chest+structure | ground | distant terrain | mountain):

    native + FXAA   31.5/0.96/1.9 | 35.6/0.94/0.1 | 36.5/0.98/3.2 | 35.2/0.93/1.6
    FSR 3.1 Quality 31.2/0.94/1.9 | 29.8/0.99/1.2 | 32.6/1.01/1.2 | 33.6/0.87/0.7
    FSR 4   Quality 28.4/0.87/1.0 | 30.8/0.89/2.0 | 35.4/0.94/1.9 | 35.3/0.92/1.3
    FSR 4   Quality, colour not flagged gamma 28.3/0.93/1.3 | 30.7/0.91/3.0 | 32.6/1.01/2.7 | 33.9/0.93/1.9
    FSR 3.1 NativeAA 28.1/1.18/0.7 | 28.8/1.20/0.8 | 33.9/1.37/1.2 | 35.1/1.32/0.8
    FSR 4   NativeAA 28.4/0.95/0.8 | 33.0/0.99/1.5 | 30.5/1.08/2.0 | 33.6/1.00/1.0

Reading: it is not a clear win on this metric in a static, already clean scene: FSR 4 Quality is closer to the
supersampled picture than FSR 3.1 Quality on the distant terrain and the mountain (+2.8 / +1.7 dB) and on the ground
(+1.0 dB), worse on the chest / structure (-2.8 dB) and a little softer (sharpness 0.87-0.94 vs 0.87-1.01) at Sharpness
0.8. FSR 4 NativeAA keeps the picture as sharp as the reference (0.95-1.08) where FSR 3.1 NativeAA over-sharpens (1.18-1.37 at the
same Sharpness); its PSNR is higher on the ground (+4.2) and mountain, lower on the distant terrain (-3.4). With
the gamma flags the colour is closer on the ground / terrain / mountain; flicker is the same order for both. One sample
per setting, one scene: treat the numbers as indications. The crops are in the pull request (they are not kept in the repository); `FSR-PORT.md`, section "Comparison images", describes them.

Seen in the pan frames (camera turning right, hunter walking in place): no ghost trails behind the hunter or the
shield, thin features (chest straps, rope, hunter's outline) stable; no jitter-sign problem (a wrong sign shows as
shimmer on static detail; the standing frames flicker like FSR 3.1's).

## Verified

- Loaded the real driver provider in game (`FSR 4.1.2`, `isSupported` = true, context created in 66-73 ms), 60 FSR 4
  frames a second in the quest scene; menus / title / loading are scaled as with FSR 3.1.
- Fallback: `MHGU_FSR4_DLL=C:/nonexistent/...` -> `FSR 3.1 is used`, FSR 3.1 runs (60 FSR frames a second);
  `MHGU_FSR4_FAILTEST=100` -> `the FSR 4 dispatch failed ... FSR 3.1 takes over` at the 100th dispatch, FSR continues with
  FSR 3.1 the next frame (one history reset, 59-60 FSR frames a second after).
- Vulkan validation (`MHGU_VK_VALIDATION=1`, SDK 1.4.341.1 layer) with FSR 4 in the quest: no message from our own
  commands (barriers, layouts, copies). What remains comes from AMD's own shader modules and the game's old messages:
  the provider's SPIR-V declaring NV cooperative-matrix-2 capabilities (11 `spirv-val` errors, 9 of them the same one) and
  storage-image sampled-type width mismatches in its RCAS / reprojection shaders (7 messages), plus the two old renderer
  messages (a 1D image view, a fragment output without attachment). The driver compiles and runs them (frames are right).

## Verified for the routes (final branch, 25 s quest-101 runs each, RX 9070 XT)

- `FSR4_DRIVER_PATCH=1` (not the default): driver route, `FSR 4.1.2`, 60 FSR 4 frames a second, no exception. With `MHGU_FSR4_ROUTE=loader` and the v1.0.1 loader: `this loader offers no FSR 4`, FSR 3.1 ran.
- `FSR4_DRIVER_PATCH=0` (the default): no loader supplied: `no loader supplied`, `no FSR 4 route available: FSR 3.1 is used`, FSR 3.1 ran; with the v1.0.1 loader: same fallback. No exception.
- Not verified: the loader route with a loader that offers FSR 4 (none available).

## Not done / known issues

- Only tested with the one provider this driver gives (4.1.2), only the Vulkan renderer on Windows, an RX 9070 XT.
  Other GPUs: the provider's `isSupported` decides (FSR 4 needs cooperative matrix + float8; RDNA 3 uses INT8 models in
  the driver package, untested).
- Linux: unconfirmed whether FSR 4 on Vulkan runs there. FSR 4 is expected to come to Linux, but whether AMD's DLL route or
  a similar trick works there is not confirmed yet. In this build the FSR 4 code is compiled out on non-Windows and
  FSR 3.1 is used (the FSR 3.1 code was syntax-checked for Linux with gcc, not linked or run).
- Tested by the author in game since: a large monster fight (no ghosting or shimmer seen), the village. Not tested:
  4K, multiplayer.
- Loading uses a hook on the driver DLL's import table (see above); a driver whose DLL checks other modules would refuse
  (then: `the driver gave no upscaler provider`, FSR 3.1). The driver DLL is trusted code loaded from the driver store
  (or beside the exe: do not put a copy there you do not trust).
- FSR 4 on the AMD driver recreates its network (pipelines) when the ratio changes; the context is created once so
  this port never does. Changing Upscaler / FSRQuality needs a restart (as FSR 3.1's quality already did).
- The colour input is gamma-encoded LDR: FSR 4 was designed for that as well as HDR; `MHGU_FSR4_LINEAR=1` compares.
- The FSR 4 GPU cost in the table includes the shared prep / finish passes (~0.5 ms at 720p), measured with the
  renderer's gpuprof, not with the provider's own timestamps.
- `DX12 bridge`: not built (not needed: the driver's Vulkan provider works).
