// The upscaler plugin host (runtime/upscaler_plugin.h): [Upscaler] Upscaler = FSR4 uses the upscaler a mod brings as a native
// library (FSR 4 is one: plugins/fsr4, built apart from the program), dispatched where FSR 3.1's would be. Without a
// plugin, or when it cannot start on this GPU, everything here says "no" and fsr.c keeps using FSR 3.1.
#pragma once
#include "vk.h"
#include "../../upscaler_plugin.h"

typedef UpscalerImage Fsr4Image;
typedef UpscalerDispatch Fsr4Dispatch;

// device.c, only for FSR on + Upscaler = FSR4 (the settings name the second choice FSR4 for now):
uint32_t vk_fsr4_api_version(void);  // before the instance: finds and loads the plugin; the API version it needs (0: none / the core's)
// create_device: the plugin's device extensions / features. exts: before the extension list is used up (room for 8 more);
// features: once the feature chain exists. false when there is no plugin or the GPU is not for it (FSR 3.1 runs).
bool vk_fsr4_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext);
bool vk_fsr4_device_features(VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12,
                             VkPhysicalDeviceFeatures *f);

bool vk_fsr4_init(void);    // fsr.c vk_fsr_init, device exists: false: FSR 3.1
bool vk_fsr4_ready(void);   // loaded and has not failed (yet)
const char *vk_fsr4_name(void);  // "FSR 4.1.2"
// Records the upscale into d->cb. false: it failed, the plugin is off from now on (logged), the caller falls back to FSR 3.1.
bool vk_fsr4_dispatch(const Fsr4Dispatch *d);
