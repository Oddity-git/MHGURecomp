// The plugin interface: a mod may bring native code as a library (a "plugin"). Drop-in, like Forge's plugins: a mod folder with
// plugins/*.dll (Windows) or plugins/*.so (Linux) loads them whenever the mod is turned on in the Mods tab, with nothing to set up.
// A plugin says what it provides (today: an upscaler, used instead of FSR 3.1 when [Upscaler] Upscaler = FSR4); the core knows
// nothing else about it. FSR 4 (plugins/fsr4) is one, and ships as a mod, not as part of the program.
//
// The library exports one function:
//     const MhguPlugin *mhgu_plugin_get(uint32_t host_abi);
// which returns NULL if it does not know that host_abi. A plugin that provides nothing this program knows is ignored. All calls come from the render thread (device_exts and
// device_features from the thread that creates the Vulkan device), never at the same time.
//
// Order: device_exts (instance and physical device exist) -> device_features -> the device is created -> init (device
// exists) -> dispatch every frame the plugin is used. Anything a plugin wants beyond this program's baseline (extensions,
// features) has to be asked for in the first two, since the device cannot be changed afterwards. Failing is always allowed:
// the core logs it and goes on with FSR 3.1. After dispatch returns false it is not called again.
//
// Native code in a mod runs with the player's rights, like any program: only install plugins from people you trust. The log
// names each one loaded; turning the mod off keeps it from loading.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define MHGU_PLUGIN_ABI 1  // changes (and stays 1 until then) only when this header does in a way old plugins cannot follow

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} UpscalerImage;

// The same inputs FSR 3.1 gets. All images live in VK_IMAGE_LAYOUT_GENERAL; record into cb, do not submit.
typedef struct {
    VkCommandBuffer cb;
    UpscalerImage color, depth, motion, reactive, output;  // reactive.image == VK_NULL_HANDLE: none
    float jitter[2];                                       // the picture shift in render pixels (as for FSR 3.1)
    uint32_t render[2], out[2];
    float frame_ms, cam_near, cam_far, fovy, view_to_meters, sharpness;
    bool sharpen, reset;
} UpscalerDispatch;

typedef struct UpscalerHost {
    uint32_t abi;
    void (*log)(const char *line);  // one line for log.txt (the plugin writes "[fsr4] ..." itself; a newline is added)
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;                // VK_NULL_HANDLE until init
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkGetDeviceProcAddr get_device_proc;
    uint64_t (*now_ns)(void);       // monotonic
    const char *dir;               // the folder the plugin library is in (UTF-8): look for its own files there
    bool debug;                    // MHGU_FSR_DEBUG is set
} UpscalerHost;

typedef struct UpscalerPlugin {
    uint32_t vulkan_api;            // the Vulkan API version the instance needs, e.g. VK_API_VERSION_1_3 (0: the core's own); read
                                    // before the instance is created, so nothing but get() runs before it
    // The device extensions to add (append to ext, up to 8 more) and the core's feature chains
    // to extend (f11, f12 and f are what vkCreateDevice gets; chain more structures behind f12->pNext). ep: what the GPU
    // offers. false: this GPU is not for this plugin.
    bool (*device_exts)(const UpscalerHost *h, const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext);
    bool (*device_features)(const UpscalerHost *h, VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12,
                            VkPhysicalDeviceFeatures *f);
    bool (*init)(const UpscalerHost *h);        // false: not available (the reason logged by the plugin)
    const char *(*name)(void);                  // what the log and the overlay call it: "FSR 4.1.2"
    bool (*dispatch)(const UpscalerDispatch *d);  // false: failed (logged), off from now on
    // Why the player gets FSR 3.1, shown in the settings menu next to the setting (English; "" or NULL: no reason to give).
    const char *(*fallback_reason)(void);
    void (*shutdown)(void);                     // optional
} UpscalerPlugin;

// What a plugin library returns from mhgu_plugin_get. More kinds of things a plugin can provide are added as more members
// at the end (the ABI number moves only when an existing member changes).
typedef struct MhguPlugin {
    uint32_t abi;                    // MHGU_PLUGIN_ABI
    const char *name;                // for the log and the Mods tab
    const UpscalerPlugin *upscaler;  // NULL: none
} MhguPlugin;
