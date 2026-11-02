# FSR 4 upscaler mod

FSR 4 is not part of the program. It is an *upscaler plugin*: a native library a mod brings, which the program uses when
`[Upscaler] Upscaler = FSR4` (interface: `runtime/upscaler_plugin.h`). The plugin here uses AMD's FSR SDK 2.4.0 DLLs.

## Build (Windows)

    make fsr4-plugin

makes `build/mods/FSR4/` with `upscaler/mhgu_fsr4.dll`, AMD's `amd_fidelityfx_loader_vk.dll` and
`amd_fidelityfx_upscaler_vk.dll` (downloaded from AMD's v2.4.0 release, SHA-256 checked) and AMD's licence text. Copy the `FSR4`
folder into `mods/` and leave it turned on in the Mods tab. It needs Vulkan SDK headers that know `VK_EXT_shader_float8`.

## What it does

- Asks for what the models need when the GPU is set up (cooperative matrix, float8, integer dot products, Vulkan 1.3).
- Loads AMD's loader from its own folder, asks which upscaler versions this GPU gets, takes the newest 4.x.
- If that fails, or the mod is missing, FSR 3.1 is used (the reason is in log.txt, `[fsr4]` lines, and under the setting).

`MHGU_UPSCALER_PLUGIN=<path>` loads a plugin library from a path instead (for development); `MHGU_FSR4_LOADER=<path>` another
AMD loader; `MHGU_FSR4_LINEAR=1` and `MHGU_FSR_DEBUG=1` as before.

## Trust

A plugin is native code and runs with the player's rights. Install ones from people you trust. The program loads one only when
the player picked FSR4 as the upscaler.
