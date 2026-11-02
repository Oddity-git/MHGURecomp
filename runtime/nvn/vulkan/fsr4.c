// The upscaler plugin host (fsr4.h, runtime/upscaler_plugin.h): finds a plugin library in the mods that are on, loads it and
// calls it where the renderer would run FSR 3.1. Everything FSR 4 specific is in the plugin (plugins/fsr4).
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../../rt.h"
#include "../../modorder.h"
#include "fsr4.h"

#ifdef _WIN32
#include <windows.h>
#define PLUGIN_EXT ".dll"
#else
#include <dlfcn.h>
#define PLUGIN_EXT ".so"
#endif

#define NO_PLUGIN "FSR 4 comes as a mod: put its upscaler mod in the mods folder and turn it on. FSR 3.1 is used."
#define NO_GPU "This GPU can't run FSR 4, so FSR 3.1 is used."
#define NO_START "FSR 4 could not start, so FSR 3.1 is used. The [fsr4] lines in log.txt say why."

static const UpscalerPlugin *pl;
static UpscalerHost host;
static bool searched, started, failed;
static char dir[1100];

static void host_log(const char *line) { LOG("%s\n", line); }

// Every plugin library in <mods>/<mod>/plugins/ of the mods that are on, in load order: the first one that provides an upscaler
// is the upscaler (MHGU_PLUGIN=<path> loads one more, for a plugin being developed).
static bool try_plugin(const char *path) {
    typedef const MhguPlugin *(*GetFn)(uint32_t);
    GetFn get = NULL;
#ifdef _WIN32
    wchar_t wp[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH);
    for (wchar_t *c = wp; *c; c++) if (*c == L'/') *c = L'\\';
    HMODULE h = LoadLibraryExW(wp, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);  // (its own DLLs are looked up beside it)
    if (h) get = (GetFn)(void *)GetProcAddress(h, "mhgu_plugin_get");
    if (!h) LOG("[plugins] %s could not be loaded (error %lu)\n", path, GetLastError());
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) get = (GetFn)dlsym(h, "mhgu_plugin_get");
    if (!h) LOG("[plugins] %s could not be loaded: %s\n", path, dlerror());
#endif
    if (!get) {
        if (h) LOG("[plugins] %s has no mhgu_plugin_get: not a plugin of this program (skipped)\n", path);
        return false;
    }
    const MhguPlugin *p = get(MHGU_PLUGIN_ABI);
    if (!p || p->abi != MHGU_PLUGIN_ABI) {
        LOG("[plugins] %s does not fit this program's plugin interface (version %d): skipped\n", path, MHGU_PLUGIN_ABI);
        return false;
    }
    LOG("[plugins] %s: %s%s\n", path, p->name ? p->name : "(unnamed)", p->upscaler ? " (upscaler)" : "");
    const UpscalerPlugin *u = p->upscaler;
    if (pl || !u) return false;
    if (!u->device_exts || !u->device_features || !u->init || !u->name || !u->dispatch) {
        LOG("[plugins] %s: its upscaler is incomplete: skipped\n", path);
        return false;
    }
    snprintf(dir, sizeof dir, "%s", path);
    char *s = strrchr(dir, '/');
    if (s) *s = 0;
    pl = u;
    return true;
}

static void scan_plugins(void) {
    const char *own = getenv("MHGU_PLUGIN");
    if (own && *own) try_plugin(own);
    ModEntry all[MODS_MAX];
    int cnt = mods_installed(all, MODS_MAX);
    for (int i = 0; i < cnt; i++) {
        if (!all[i].on) continue;
        char d[1100];
        snprintf(d, sizeof d, "%s/%s/plugins", mods_dir(), all[i].name);
        DIR *dp = opendir(d);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            size_t l = strlen(e->d_name), x = strlen(PLUGIN_EXT);
            char p[1400];
            if (l > x && !strcmp(e->d_name + l - x, PLUGIN_EXT)) {
                snprintf(p, sizeof p, "%s/%s", d, e->d_name);
                try_plugin(p);
            }
        }
        closedir(dp);
    }
}

static const UpscalerPlugin *load_plugin(void) {
    if (searched) return pl;
    searched = true;
    scan_plugins();
    if (!pl) {
        LOG("[fsr4] no upscaler plugin in the mods that are on (mods/<mod>/plugins/*%s): FSR 3.1 is used\n", PLUGIN_EXT);
        g_fsr4_fallback = NO_PLUGIN;
    }
    return pl;
}

static void reason(void) {
    const char *r = pl && pl->fallback_reason ? pl->fallback_reason() : NULL;
    g_fsr4_fallback = r && *r ? r : NO_START;
}

uint32_t vk_fsr4_api_version(void) { return load_plugin() ? pl->vulkan_api : 0; }

bool vk_fsr4_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext) {
    if (!load_plugin()) return false;
    host = (UpscalerHost){.abi = MHGU_PLUGIN_ABI, .log = host_log, .instance = g_vk.inst, .phys = g_vk.phys,
                          .get_instance_proc = vkGetInstanceProcAddr, .get_device_proc = vkGetDeviceProcAddr,
                          .now_ns = plat_now_ns, .dir = dir, .debug = getenv("MHGU_FSR_DEBUG") != NULL};
    uint32_t before = *n_ext;
    if (!pl->device_exts(&host, ep, n, ext, n_ext)) {
        *n_ext = before;
        g_fsr4_fallback = NO_GPU;
        reason();
        return false;
    }
    started = true;  // (the device gets the plugin's extensions: features next)
    return true;
}

bool vk_fsr4_device_features(VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12,
                             VkPhysicalDeviceFeatures *f) {
    if (!pl || !started) return false;
    if (!pl->device_features(&host, f11, f12, f)) {
        started = false;
        g_fsr4_fallback = NO_GPU;
        reason();
        return false;
    }
    return true;
}

bool vk_fsr4_init(void) {
    if (!pl || !started) return false;
    host.device = g_vk.dev;
    if (!pl->init(&host)) {
        LOG("[fsr4] the plugin could not start: FSR 3.1 is used\n");
        reason();
        failed = true;
        return false;
    }
    return true;
}

bool vk_fsr4_ready(void) { return pl && started && !failed; }
const char *vk_fsr4_name(void) { return pl && pl->name ? pl->name() : "FSR 4"; }

bool vk_fsr4_dispatch(const Fsr4Dispatch *d) {
    if (!vk_fsr4_ready()) return false;
    static int fail_at = -2, frames;  // (test: MHGU_FSR4_FAILTEST=n reports the n-th dispatch as failed, to see the fallback)
    if (fail_at == -2) fail_at = getenv("MHGU_FSR4_FAILTEST") ? atoi(getenv("MHGU_FSR4_FAILTEST")) : -1;
    if ((fail_at >= 0 && frames++ == fail_at) || !pl->dispatch(d)) {
        LOG("[fsr4] the upscaler plugin failed: FSR 3.1 takes over from here on\n");
        reason();
        failed = true;
        return false;
    }
    return true;
}
