// FSR 4 as an upscaler plugin (runtime/upscaler_plugin.h): a native library that goes into a mod, mods/<mod>/upscaler/, with
// AMD's FSR SDK 2.4 DLLs beside it. It is built apart from the program (make fsr4-plugin) and is not part of it.
//
// AMD's FSR SDK 2.4 ships signed Vulkan DLLs: amd_fidelityfx_loader_vk.dll (the public ffx-api) and, beside it,
// amd_fidelityfx_upscaler_vk.dll (the FSR 4 ML upscaler among the providers). This plugin loads the loader from its own
// folder (or MHGU_FSR4_LOADER=<path>), asks it for the upscaler versions (ffxQuery) and takes the newest 4.x one
// (ffxCreateContext / ffxDispatch), which then runs on the program's VkDevice and records into its VkCommandBuffer. The
// structures are those of the MIT-licensed ffx-api headers (plugins/fsr4/ffx-api, FSR SDK 2.4.0). The DLLs are AMD's,
// unmodified, and not linked in. If they are missing or FSR 4 cannot start, the program uses FSR 3.1.
//
//   MHGU_FSR4_LINEAR=1   A/B: the colour is not flagged as gamma-encoded
#define WIN32_LEAN_AND_MEAN
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "../../runtime/upscaler_plugin.h"

#define EXPORT __declspec(dllexport)
static const UpscalerHost *H;
static void LOG(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    if (H && H->log) H->log(b);
}
static uint64_t plat_now_ns(void) { return H->now_ns(); }
static const char *g_reason = "";
#define NO_FSR4_GPU "This GPU can't run FSR 4, so FSR 3.1 is used."
#define NO_FSR4_START "FSR 4 could not start, so FSR 3.1 is used. The [fsr4] lines in log.txt say why."

#include <windows.h>

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#define FSR4_VERSION FFX_UPSCALER_VERSION  // (4.1.2 in the 2.4 headers)

static char prov_name[64];
// the three calls the dispatch code uses, from either route
static ffxReturnCode_t (*f_create)(ffxContext *, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *);
static ffxReturnCode_t (*f_destroy)(ffxContext *, const ffxAllocationCallbacks *);
static ffxReturnCode_t (*f_dispatch)(ffxContext *, const ffxDispatchDescHeader *);
static uint64_t loader_ver;   // the FSR 4 version id the loader offered
static ffxContext ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static bool loaded, failed, extensions_ok, linear_in;  // linear_in: MHGU_FSR4_LINEAR=1 (A/B: the colour is not flagged as gamma-encoded)
static uint64_t frames;

// ================================================================== the device
// What the GPU must have for any FSR 4 model: the rest of the list is optional and switched on when the GPU offers it. The
// models differ per GPU (RDNA 4: float8 + cooperative matrix; RDNA 2 / 3: the INT8 models, which want cooperative matrix
// (RDNA 3) or integer dot products), and AMD's loader / upscaler DLL is what decides whether this GPU gets FSR 4 at all (the
// version query in init_loader, then the context). The device is created before the loader is asked, so everything the models
// may use has to be enabled here; unused extensions and features are harmless.
static bool have_cm, have_f8, have_dp;

static bool p_device_exts(const UpscalerHost *h, const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext) {
    H = h;
    static const char *const need[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME};
    bool has[2] = {false, false};
    have_cm = have_f8 = have_dp = false;
    for (uint32_t j = 0; j < n; j++) {
        for (int i = 0; i < 2; i++) has[i] |= !strcmp(ep[j].extensionName, need[i]);
        have_cm |= !strcmp(ep[j].extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        have_f8 |= !strcmp(ep[j].extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        have_dp |= !strcmp(ep[j].extensionName, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    }
    for (int i = 0; i < 2; i++) {
        if (has[i]) continue;
        LOG("[fsr4] the GPU lacks %s: FSR 4 cannot run here, FSR 3.1 will\n", need[i]);
        g_reason = NO_FSR4_GPU;
        return false;
    }
    for (int i = 0; i < 2; i++) ext[(*n_ext)++] = need[i];
    if (have_cm) ext[(*n_ext)++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    if (have_f8) ext[(*n_ext)++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
    if (have_dp) ext[(*n_ext)++] = VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME;
    extensions_ok = true;
    return true;
}

static VkPhysicalDeviceCooperativeMatrixFeaturesKHR en_cm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
static VkPhysicalDeviceShaderFloat8FeaturesEXT en_f8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
static VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR en_dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR};
static VkPhysicalDeviceSynchronization2FeaturesKHR en_s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
static VkPhysicalDeviceSubgroupSizeControlFeaturesEXT en_sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};

static bool p_device_features(const UpscalerHost *h, VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12,
                              VkPhysicalDeviceFeatures *f) {
    H = h;
    if (!extensions_ok) return false;
    // what the GPU reports, for the optional structures only those whose extension is enabled
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {.sType = en_cm.sType};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {.sType = en_f8.sType};
    VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR dp = {.sType = en_dp.sType};
    VkPhysicalDeviceSynchronization2FeaturesKHR s2 = {.sType = en_s2.sType};
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sg = {.sType = en_sg.sType};
    void *chain = NULL;
    sg.pNext = chain, chain = &sg;
    s2.pNext = chain, chain = &s2;
    if (have_cm) cm.pNext = chain, chain = &cm;
    if (have_f8) f8.pNext = chain, chain = &f8;
    if (have_dp) dp.pNext = chain, chain = &dp;
    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = chain};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    VkPhysicalDeviceFeatures2 hf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)H->get_instance_proc(H->instance, "vkGetPhysicalDeviceFeatures2");
    if (!get_features2) { LOG("[fsr4] vkGetPhysicalDeviceFeatures2 is missing\n"); extensions_ok = false; g_reason = NO_FSR4_GPU; return false; }
    get_features2(H->phys, &hf);
    bool use_cm = have_cm && cm.cooperativeMatrix, use_f8 = have_f8 && f8.shaderFloat8, use_dp = have_dp && dp.shaderIntegerDotProduct;
    LOG("[fsr4] GPU features: cooperative matrix %d, float8 %d/%d, integer dot product %d\n", use_cm, f8.shaderFloat8,
        f8.shaderFloat8CooperativeMatrix, use_dp);
    if (!s2.synchronization2 || !sg.subgroupSizeControl || !sg.computeFullSubgroups || !h12.shaderFloat16 || !h12.shaderInt8 ||
        !h12.bufferDeviceAddress || !h12.vulkanMemoryModel || !hf.features.shaderInt16 || !h11.storageBuffer16BitAccess) {
        LOG("[fsr4] the GPU lacks a feature FSR 4 needs (synchronization2 %d, subgroup size control %d/%d, fp16 %d, int8 %d, "
            "buffer device address %d, memory model %d, int16 %d, 16-bit storage %d): FSR 3.1 will run\n", s2.synchronization2,
            sg.subgroupSizeControl, sg.computeFullSubgroups, h12.shaderFloat16, h12.shaderInt8, h12.bufferDeviceAddress,
            h12.vulkanMemoryModel, hf.features.shaderInt16, h11.storageBuffer16BitAccess);
        extensions_ok = false;  // (the extensions stay enabled: harmless without their features)
        g_reason = NO_FSR4_GPU;
        return false;
    }
    if (!use_cm && !use_f8 && !use_dp) {
        LOG("[fsr4] the GPU offers none of cooperative matrix, float8 or integer dot products: FSR 3.1 will run\n");
        extensions_ok = false;
        g_reason = NO_FSR4_GPU;
        return false;
    }
    if (use_cm) en_cm.cooperativeMatrix = en_cm.cooperativeMatrixRobustBufferAccess = cm.cooperativeMatrix ? VK_TRUE : VK_FALSE;
    if (use_f8) en_f8.shaderFloat8 = VK_TRUE, en_f8.shaderFloat8CooperativeMatrix = f8.shaderFloat8CooperativeMatrix;
    if (use_dp) en_dp.shaderIntegerDotProduct = VK_TRUE;
    en_s2.synchronization2 = VK_TRUE;
    en_sg.subgroupSizeControl = en_sg.computeFullSubgroups = VK_TRUE;
    f12->shaderFloat16 = f12->shaderInt8 = f12->bufferDeviceAddress = f12->vulkanMemoryModel = VK_TRUE;
    f12->vulkanMemoryModelDeviceScope = h12.vulkanMemoryModelDeviceScope;
    f12->shaderSubgroupExtendedTypes = h12.shaderSubgroupExtendedTypes;
    f12->uniformAndStorageBuffer8BitAccess = f12->storageBuffer8BitAccess = VK_TRUE;
    f12->descriptorIndexing = h12.descriptorIndexing;
    f12->runtimeDescriptorArray = h12.runtimeDescriptorArray;
    f12->descriptorBindingPartiallyBound = h12.descriptorBindingPartiallyBound;
    f12->shaderSampledImageArrayNonUniformIndexing = h12.shaderSampledImageArrayNonUniformIndexing;
    f12->shaderStorageImageArrayNonUniformIndexing = h12.shaderStorageImageArrayNonUniformIndexing;
    f11->storageBuffer16BitAccess = f11->uniformAndStorageBuffer16BitAccess = VK_TRUE;
    f11->variablePointers = h11.variablePointers;
    f11->variablePointersStorageBuffer = h11.variablePointersStorageBuffer;
    f->shaderInt16 = VK_TRUE;
    f->shaderStorageImageReadWithoutFormat = f->shaderStorageImageWriteWithoutFormat = VK_TRUE;
    // splice the extension structures in after the 1.2 features (whatever f12 pointed to stays behind them)
    void *rest = f12->pNext;
    en_sg.pNext = rest, rest = &en_sg;
    en_s2.pNext = rest, rest = &en_s2;
    if (have_cm) en_cm.pNext = rest, rest = &en_cm;
    if (have_f8) en_f8.pNext = rest, rest = &en_f8;
    if (have_dp) en_dp.pNext = rest, rest = &en_dp;
    f12->pNext = rest;
    return true;
}

static void file_version(const wchar_t *path, char *out, size_t n) {
    snprintf(out, n, "?");
    DWORD z, sz = GetFileVersionInfoSizeW(path, &z);
    if (!sz) return;
    void *buf = malloc(sz);
    VS_FIXEDFILEINFO *fi;
    UINT l;
    if (buf && GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &l) && fi)
        snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                 LOWORD(fi->dwFileVersionLS));
    free(buf);
}

// ================================================================== the loader (AMD's, bundled beside the exe)
static bool init_loader(void) {
    wchar_t path[MAX_PATH] = {0};
    const char *env = getenv("MHGU_FSR4_LOADER");
    if (env) MultiByteToWideChar(CP_UTF8, 0, env, -1, path, MAX_PATH);
    else if (H->dir && MultiByteToWideChar(CP_UTF8, 0, H->dir, -1, path, MAX_PATH)) {  // beside this plugin
        for (wchar_t *c = path; *c; c++) if (*c == L'/') *c = L'\\';
        wcscat(path, L"\\amd_fidelityfx_loader_vk.dll");
    }
    if (!path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LOG("[fsr4] amd_fidelityfx_loader_vk.dll not found (beside the plugin, or MHGU_FSR4_LOADER=<path>): FSR 3.1 is used\n");
        return false;
    }
    HMODULE ld = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ld) { LOG("[fsr4] %ls could not be loaded (error %lu)\n", path, GetLastError()); return false; }
    char ver[32];
    file_version(path, ver, sizeof ver);
    LOG("[fsr4] loader %ls, file version %s\n", path, ver);
    PfnFfxQuery query = (PfnFfxQuery)GetProcAddress(ld, "ffxQuery");
    f_create = (void *)GetProcAddress(ld, "ffxCreateContext");
    f_destroy = (void *)GetProcAddress(ld, "ffxDestroyContext");
    f_dispatch = (void *)GetProcAddress(ld, "ffxDispatch");
    if (!query || !f_create || !f_destroy || !f_dispatch) { LOG("[fsr4] the loader lacks the ffx-api exports\n"); return false; }
    VkDeviceContext dc = {.vkDevice = H->device, .vkPhysicalDevice = H->phys, .vkDeviceProcAddr = H->get_device_proc};
    uint64_t n = 16, ids[16] = {0};
    const char *names[16] = {0};
    struct ffxQueryDescGetVersions gv = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                         .device = &dc, .outputCount = &n, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t rc = query(NULL, &gv.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[fsr4] the loader's version query failed (ffx return code %d; is amd_fidelityfx_upscaler_vk.dll beside it?)\n", (int)rc);
        return false;
    }
    int best = -1;
    for (uint64_t i = 0; i < n; i++) {
        LOG("[fsr4] loader offers upscaler %s (id %#llx)\n", names[i] ? names[i] : "?", (unsigned long long)ids[i]);
        if (names[i] && names[i][0] == '4' && names[i][1] == '.' && (best < 0 || strcmp(names[i], names[best]) > 0)) best = (int)i;
    }
    if (best < 0) { LOG("[fsr4] this loader offers no FSR 4 (4.x) upscaler on this GPU\n"); return false; }
    loader_ver = ids[best];
    snprintf(prov_name, sizeof prov_name, "FSR %s", names[best]);
    return true;
}

static bool p_init(const UpscalerHost *h) {
    H = h;
    if (!extensions_ok) return false;
    if (!init_loader()) {
        LOG("[fsr4] no FSR 4 available: FSR 3.1 is used\n");
        g_reason = NO_FSR4_START;
        return false;
    }
    LOG("[fsr4] route: AMD's FSR SDK loader (ffx-api)\n");
    linear_in = getenv("MHGU_FSR4_LINEAR") != NULL;
    loaded = true;
    return true;
}

static const char *p_name(void) { return prov_name[0] ? prov_name : "FSR 4"; }
static const char *p_reason(void) { return g_reason; }

// ================================================================== context and dispatch
static void ffx_message(uint32_t type, const wchar_t *msg) {
    LOG("[fsr4] runtime %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", msg);
}

static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// every image lives in GENERAL here (vk.h), which is what "unordered access" means to the backend: no layout changes
static struct FfxApiResource resource(const UpscalerImage *i, bool uav) {
    struct FfxApiResource r = {0};
    if (!i->image) return r;
    r.resource = (void *)i->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format(i->format);
    r.description.width = i->width, r.description.height = i->height, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return r;
}

static bool fail(const char *why, int rc) {
    LOG("[fsr4] %s (ffx return code %d): FSR 4 is off from here on, FSR 3.1 takes over\n", why, rc);
    failed = true;
    g_reason = NO_FSR4_START;
    return false;
}

static bool make_context(const UpscalerDispatch *d) {
    if (ctx && ctx_rw == d->render[0] && ctx_rh == d->render[1] && ctx_ow == d->out[0] && ctx_oh == d->out[1]) return true;
    if (ctx) { f_destroy(&ctx, NULL); ctx = NULL; }
    // LDR, gamma-encoded colour (the post-processed picture), standard depth, render-size motion vectors without jitter
    struct ffxCreateContextDescUpscale cu = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                      .flags = (linear_in ? 0 : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) | (H->debug ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0),
                                      .maxRenderSize = {d->render[0], d->render[1]}, .maxUpscaleSize = {d->out[0], d->out[1]},
                                      .fpMessage = ffx_message};
    struct ffxCreateContextDescUpscaleVersion uv = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, .version = FSR4_VERSION};
    struct ffxCreateBackendVKDesc bd = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
                                        .deviceContext = {H->device, H->phys, H->get_device_proc}};
    struct ffxOverrideVersion ov = {.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION, .versionId = loader_ver};
    bd.header.pNext = &uv.header;
    uv.header.pNext = &ov.header;
    cu.header.pNext = &bd.header;
    uint64_t t0 = plat_now_ns();
    ffxReturnCode_t rc = f_create(&ctx, &cu.header, NULL);
    if (rc != FFX_API_RETURN_OK || !ctx) { ctx = NULL; return fail("the FSR 4 context could not be created", (int)rc); }
    ctx_rw = d->render[0], ctx_rh = d->render[1], ctx_ow = d->out[0], ctx_oh = d->out[1];
    LOG("[fsr4] %s context %ux%u -> %ux%u created in %.0f ms (non-linear colour; colour VkFormat %d, depth %d, motion vectors %d, "
        "reactive %d, output %d)\n", prov_name, ctx_rw, ctx_rh, ctx_ow, ctx_oh, (plat_now_ns() - t0) / 1e6, d->color.format,
        d->depth.format, d->motion.format, d->reactive.image ? d->reactive.format : 0, d->output.format);
    return true;
}

static bool p_dispatch(const UpscalerDispatch *d) {
    if (!loaded || failed || !make_context(d)) return false;
    struct ffxDispatchDescUpscale du = {.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = d->cb};
    du.color = resource(&d->color, false);
    du.depth = resource(&d->depth, false);
    du.motionVectors = resource(&d->motion, false);
    du.reactive = resource(&d->reactive, false);
    du.output = resource(&d->output, true);
    du.jitterOffset = (struct FfxApiFloatCoords2D){d->jitter[0], d->jitter[1]};
    du.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    du.renderSize = (struct FfxApiDimensions2D){d->render[0], d->render[1]};
    du.upscaleSize = (struct FfxApiDimensions2D){d->out[0], d->out[1]};
    du.enableSharpening = d->sharpen;
    du.sharpness = d->sharpness;
    du.frameTimeDelta = d->frame_ms;
    du.preExposure = 1.0f;
    du.reset = d->reset;
    du.cameraNear = d->cam_near;
    du.cameraFar = d->cam_far;
    du.cameraFovAngleVertical = d->fovy;
    du.viewSpaceToMetersFactor = d->view_to_meters;
    du.flags = linear_in ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    ffxReturnCode_t rc = f_dispatch(&ctx, &du.header);
    if (rc != FFX_API_RETURN_OK) return fail("the FSR 4 dispatch failed", (int)rc);
    if (++frames == 1) LOG("[fsr4] first %s dispatch recorded\n", prov_name);
    return true;
}

static const UpscalerPlugin plugin = {
    .abi = MHGU_UPSCALER_ABI,
    .vulkan_api = VK_API_VERSION_1_3,  // (FSR 4's shaders use SPIR-V 1.6)
    .device_exts = p_device_exts,
    .device_features = p_device_features,
    .init = p_init,
    .name = p_name,
    .dispatch = p_dispatch,
    .fallback_reason = p_reason,
};

EXPORT const UpscalerPlugin *mhgu_upscaler_get(uint32_t host_abi) { return host_abi == MHGU_UPSCALER_ABI ? &plugin : NULL; }
