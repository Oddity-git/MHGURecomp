// Shader mods (post.h). A mod folder may hold shaders/post/*.glsl: compute passes over the game's 3D picture. They
// run in file name order (mods in load order, a file whose name starts with '_' is skipped), once a frame, at the
// start of the HUD (the point fsr.c finds: the first draw into the full-screen image H that is not a post pass, or
// else the final draw that samples H), on frames with a 3D scene (a depth target with >= 16 depth-writing draws and
// the main camera captured this frame or the last). With FSR on they run before it, at render size.
//
// H is copied into `original` (RGBA16F); each pass reads the picture so far and writes the next one (two RGBA16F
// images in turn); the last result is copied back into H, so the game's own HUD and brightness / gamma draw follow.
// Before the passes, a small built-in pass (LIGHT_MAIN) finds the sun or moon on screen and the sky's colour, for
// every pass to read (u_light); u_original has mipmaps (a blurred picture at higher levels).
// A pass is the text of its file between PRELUDE and EPILOGUE below: it defines vec4 effect(vec2 uv, ivec2 px), the
// colour of the texel px (uv: its centre, 0..1, y down). docs/SHADER_MODS.md lists what it can use.
//
// F6 turns the passes off and on (to compare), Shift+F6 reads the files again (to tune a pass while the game runs).
//
// Effects and settings (shader_mods.h): passes whose first line is "// Name: ..." with the same Name are one effect,
// turned on and off together. A constant with a range comment is a setting:
//     const float RADIUS = 90.0;  // [10, 300] Radius: how far round a point to look (cm)
//     const vec3 TINT = vec3(1.0, 0.9, 0.8);  // [color] Tint
// its line becomes a #define that reads the value from u_set (binding 6), so the Shader mods window (F5) changes it
// while the game runs, with no compiling. Effects' on / off and the values are kept in the mod's
// shaders/settings.ini (only the ones changed from the pass's own).
//
//   MHGU_POST_LOG=1   a line per second: frames with the passes run / passed over, and why
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "../../camera_jitter.h"
#include "../../mods.h"
#include "../../platform.h"
#include "../../rt.h"
#include "../../shader_mods.h"
#include "post.h"
#include "shaders.h"

bool g_post_on;

static const char PRELUDE[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_scene;     // the picture so far (linear filtering)\n"
    "layout(set = 0, binding = 1) uniform sampler2D u_depth;     // the scene's depth buffer, as stored (nearest)\n"
    "layout(set = 0, binding = 2) uniform sampler2D u_original;  // the game's picture before the first pass (mipmaps)\n"
    "layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D u_out;\n"
    "layout(set = 0, binding = 4, std140) uniform Frame {\n"
    "    mat4 proj, inv_proj;  // view space <-> clip space (the game's camera; view space: right, up, -z forward)\n"
    "    mat4 view, inv_view;  // world space <-> view space (world units: centimetres, y up)\n"
    "    vec4 size;            // width, height, 1 / width, 1 / height (texels)\n"
    "    vec4 info;            // seconds since start, frame number, number of passes, 0\n"
    "    vec4 cam;             // near, far (world units), vertical field of view (radians), 0\n"
    "    vec4 eye;             // the camera's position (world space), 1\n"
    "} u;\n"
    "layout(push_constant) uniform Pass { vec4 pass; } u_pass;  // x: this pass's index (0, 1, ...)\n"
    "layout(set = 0, binding = 5, std430)\n"
    "#ifndef LIGHT_PASS\n"
    "readonly\n"
    "#endif\n"
    "buffer Light {\n"
    "    vec4 pos;    // the sun / moon on screen: uv, strength (0..1), 1 = found (0: not in view)\n"
    "    vec4 color;  // its colour, the brightness the sky around it is measured against\n"
    "    vec4 sky;    // the sky's average colour (kept, fading, while no sky is in view), share of the screen\n"
    "    vec4 state;  // a slowly following height: the camera's minus 300 (world units; fog base), 1 = set\n"
    "} u_light;\n"
    "layout(set = 0, binding = 6, std140) uniform Settings { vec4 v[256]; } u_set;  // (the settings' values)\n"
    "float raw_depth(vec2 uv) { return textureLod(u_depth, uv, 0.0).r; }\n"
    "vec3 view_pos_at(vec2 uv, float d) {\n"
    "    vec4 q = u.inv_proj * vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, d, 1.0);\n"
    "    return q.xyz / q.w;\n"
    "}\n"
    "vec3 view_pos(vec2 uv) { return view_pos_at(uv, raw_depth(uv)); }\n"
    "float view_dist(vec2 uv) { return length(view_pos(uv)); }  // world units from the camera\n"
    "vec3 world_pos(vec2 uv) { return (u.inv_view * vec4(view_pos(uv), 1.0)).xyz; }\n"
    "// nothing but sky there: no depth written (cleared), or the sky dome, which the game draws as geometry about\n"
    "// 0.65-1.3 km from the camera (its backdrop mountains are nearer, within 640 m; far plane 20 km). SKY_DIST in\n"
    "// world units (cm); a pass may #undef it and #define its own.\n"
    "#define SKY_DIST 80000.0\n"
    "bool is_sky(vec2 uv) {\n"
    "    float d = raw_depth(uv);\n"
    "    return d >= 0.99999 || length(view_pos_at(uv, d)) >= SKY_DIST;\n"
    "}\n"
    "vec2 project(vec3 vp) {  // view space -> uv\n"
    "    vec4 c = u.proj * vec4(vp, 1.0);\n"
    "    vec2 n = c.xy / c.w;\n"
    "    return vec2(0.5 + 0.5 * n.x, 0.5 - 0.5 * n.y);\n"
    "}\n"
    "vec3 view_normal(vec2 uv) {  // from the depth around uv\n"
    "    vec3 c = view_pos(uv);\n"
    "    vec3 l = view_pos(uv - vec2(u.size.z, 0.0)), r = view_pos(uv + vec2(u.size.z, 0.0));\n"
    "    vec3 d = view_pos(uv - vec2(0.0, u.size.w)), t = view_pos(uv + vec2(0.0, u.size.w));\n"
    "    vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;\n"
    "    vec3 dy = abs(t.z - c.z) < abs(c.z - d.z) ? t - c : c - d;\n"
    "    return normalize(cross(dy, dx));\n"
    "}\n"
    "float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "#line 1\n";

// The built-in light pass: one 8x8 workgroup over u_original at about 64 texels across
static const char LIGHT_MAIN[] =
    "shared float s_a[64], s_b[64], s_c[64];\n"
    "shared vec4 s_v[64];\n"
    "void reduce(uint t) {  // sums s_a, s_b, s_v; maximum of s_c\n"
    "    for (uint k = 32u; k > 0u; k >>= 1) {\n"
    "        barrier();\n"
    "        if (t < k) { s_a[t] += s_a[t + k]; s_b[t] += s_b[t + k]; s_c[t] = max(s_c[t], s_c[t + k]); s_v[t] += s_v[t + k]; }\n"
    "    }\n"
    "    barrier();\n"
    "}\n"
    "void main() {\n"
    "    uint t = gl_LocalInvocationIndex;\n"
    "    int lod = int(max(floor(log2(u.size.x / 64.0)), 0.0));\n"
    "    ivec2 ls = textureSize(u_original, lod);\n"
    "    int total = ls.x * ls.y;\n"
    "    float sum = 0.0, n = 0.0, mx = 0.0;\n"
    "    vec3 rgb = vec3(0.0);\n"
    "    for (int i = int(t); i < total; i += 64) {\n"
    "        ivec2 p = ivec2(i % ls.x, i / ls.x);\n"
    "        vec2 uv = (vec2(p) + 0.5) / vec2(ls);\n"
    "        if (!is_sky(uv)) continue;\n"
    "        vec3 c = texelFetch(u_original, p, lod).rgb;\n"
    "        float l = luma(c);\n"
    "        sum += l, n += 1.0, mx = max(mx, l), rgb += c;\n"
    "    }\n"
    "    s_a[t] = sum, s_b[t] = n, s_c[t] = mx, s_v[t] = vec4(rgb, 0.0);\n"
    "    reduce(t);\n"
    "    float n_sky = s_b[0], avg = n_sky > 0.0 ? s_a[0] / n_sky : 0.0, top = s_c[0];\n"
    "    vec3 sky_rgb = n_sky > 0.0 ? s_v[0].rgb / n_sky : vec3(0.0);\n"
    "    float th = mix(avg, top, 0.7);\n"
    "    vec2 cen = vec2(0.0);\n"
    "    vec3 col = vec3(0.0);\n"
    "    float w = 0.0;\n"
    "    for (int i = int(t); i < total; i += 64) {\n"
    "        ivec2 p = ivec2(i % ls.x, i / ls.x);\n"
    "        vec2 uv = (vec2(p) + 0.5) / vec2(ls);\n"
    "        if (!is_sky(uv)) continue;\n"
    "        vec3 c = texelFetch(u_original, p, lod).rgb;\n"
    "        float k = max(luma(c) - th, 0.0);\n"
    "        cen += uv * k, col += c * k, w += k;\n"
    "    }\n"
    "    barrier();\n"
    "    s_a[t] = cen.x, s_b[t] = cen.y, s_c[t] = 0.0, s_v[t] = vec4(col, w);\n"
    "    reduce(t);\n"
    "    if (t != 0u) return;\n"
    "    float wt = s_v[0].a;\n"
    "    float contrast = top > 0.0 ? (top - avg) / top : 0.0;  // (a sun or moon is much brighter than the sky round it)\n"
    "    bool found = wt > 0.0 && n_sky > 0.0;\n"
    "    u_light.pos = found ? vec4(s_a[0] / wt, s_b[0] / wt, smoothstep(0.15, 0.5, contrast), 1.0) : vec4(0.5, 0.5, 0.0, 0.0);\n"
    "    u_light.color = vec4(found ? s_v[0].rgb / wt : vec3(1.0), th);\n"
    "    bool init = u_light.state.w > 0.5;\n"
    "    vec3 sky = n_sky > 0.0 ? sky_rgb : u_light.sky.rgb;\n"
    "    u_light.sky = vec4(init ? mix(u_light.sky.rgb, sky, 0.05) : sky, n_sky / float(total));\n"
    "    float base = u.eye.y - 300.0;\n"
    "    u_light.state = vec4(init && abs(u_light.state.x - base) < 5000.0 ? mix(u_light.state.x, base, 0.02) : base, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char EPILOGUE[] =
    "\nvoid main() {\n"
    "    ivec2 px = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (px.x >= int(u.size.x) || px.y >= int(u.size.y)) return;\n"
    "    imageStore(u_out, px, effect((vec2(px) + 0.5) * u.size.zw, px));\n"
    "}\n";

typedef struct {
    float proj[16], inv_proj[16], view[16], inv_view[16];
    float size[4], info[4], cam[4], eye[4];
} FrameU;

#define MAX_PASS 32
typedef struct {
    char name[160];  // mod/file, for the log
    VkPipeline pipe;
    int effect;
} Pass;
#define MAX_EFFECT 32
#define MAX_SET 256  // (u_set's size)
typedef struct {
    char name[64], mod[128];
    int dir;  // dirs[]: the mod's shaders/ folder (its settings.ini)
    bool on, def_on;  // on now, on unless settings.ini says otherwise ("// Name (off): ..." in a pass: off)
} Effect;
static Effect effects[MAX_EFFECT];
static int neffect;
static PostSetting sets[MAX_SET];
static int nset;
static pthread_mutex_t set_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool sets_dirty;
static uint64_t dirty_ns;
static Pass passes[MAX_PASS];
static int npass;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static TexBackend img[3];  // original (with mipmaps), then the two the passes write in turn
static VkPipeline light_pipe;
static VkBuffer light_buf;
static VmaAllocation light_mem;
static VkSampler mip_sampler;
static uint64_t t0;
static bool logging;
static const char *dirs[64];  // the mods' shaders/ folders
static int ndirs;
static atomic_bool enabled = true, reload_asked;

// ================================================================== loading
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? calloc(1, (size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    fclose(f);
    return s;
}

// The effect a pass belongs to: the name on its first line ("// Name: ...", "// Name (off): ..." for an effect that
// starts off), else its file name without the leading number and ".glsl"
static int effect_of(int dir, const char *mod, const char *file, const char *body) {
    char name[64] = "";
    if (!strncmp(body, "//", 2)) {
        const char *a = body + 2;
        while (*a == ' ') a++;
        size_t k = strcspn(a, ":\r\n");
        if (a[k] == ':' && k > 0 && k < sizeof name) snprintf(name, sizeof name, "%.*s", (int)k, a);
    }
    bool off = false;
    size_t nl = strlen(name);
    if (nl > 6 && !strcmp(name + nl - 6, " (off)")) name[nl - 6] = 0, off = true;
    if (!name[0]) {
        const char *a = file;
        while ((*a >= '0' && *a <= '9') || *a == '_') a++;
        snprintf(name, sizeof name, "%.*s", (int)(strlen(a) > 5 ? strlen(a) - 5 : 0), a);
    }
    for (int i = 0; i < neffect; i++)
        if (effects[i].dir == dir && !strcmp(effects[i].name, name)) return i;
    if (neffect >= MAX_EFFECT) return -1;
    Effect *e = &effects[neffect];
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->mod, sizeof e->mod, "%s", mod);
    e->dir = dir, e->on = e->def_on = !off;
    return neffect++;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

// A line "const <type> NAME = <value>;  // [lo, hi] Label: help" (or [color]): a setting of effect e, and the line's
// replacement (a #define reading its value from u_set). false: not a setting.
static bool setting_line(const char *line, int e, char *out, size_t on) {
    char type[16], name[48];
    int used = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (sscanf(p, "const %15s %47[A-Za-z0-9_] =%n", type, name, &used) != 2 || !used) return false;
    const char *val = p + used, *semi = strchr(val, ';');
    const char *cm = semi ? strstr(semi, "//") : NULL;
    if (!cm) return false;
    cm += 2;
    while (*cm == ' ') cm++;
    if (*cm != '[') return false;
    int kind = !strcmp(type, "float") ? POST_FLOAT : !strcmp(type, "int") ? POST_INT : !strcmp(type, "vec3") ? POST_COLOR : -1;
    if (kind < 0) return false;
    float lo = 0, hi = 1, v[3] = {0, 0, 0};
    const char *after;
    if (kind == POST_COLOR) {
        if (strncmp(cm, "[color]", 7)) return false;
        after = cm + 7;
        const char *q = strchr(val, '(');
        if (!q || sscanf(q + 1, "%f , %f , %f", &v[0], &v[1], &v[2]) != 3) return false;
    } else {
        int k = 0;
        if (sscanf(cm, "[%f , %f]%n", &lo, &hi, &k) != 2 || !k) return false;
        after = cm + k;
        v[0] = strtof(val, NULL);
    }
    int idx = -1;
    for (int i = 0; i < nset; i++)
        if (sets[i].effect == e && !strcmp(sets[i].name, name)) idx = i;
    if (idx < 0) {
        if (nset >= MAX_SET) return false;
        idx = nset++;
        PostSetting *st = &sets[idx];
        memset(st, 0, sizeof *st);
        snprintf(st->name, sizeof st->name, "%s", name);
        st->effect = e, st->type = kind, st->lo = lo, st->hi = hi;
        memcpy(st->def, v, sizeof v);
        memcpy(st->val, v, sizeof v);
        char text[300];
        size_t tl = strcspn(after, "\r\n");
        snprintf(text, sizeof text, "%.*s", (int)(tl < sizeof text ? tl : sizeof text - 1), after);
        char *t = trim(text), *colon = strchr(t, ':');
        if (colon) *colon = 0;
        snprintf(st->label, sizeof st->label, "%s", trim(t));
        if (colon) snprintf(st->help, sizeof st->help, "%s", trim(colon + 1));
        if (!st->label[0]) snprintf(st->label, sizeof st->label, "%s", name);
    }
    snprintf(out, on, kind == POST_FLOAT ? "#define %s (u_set.v[%d].x)" : kind == POST_INT ? "#define %s int(u_set.v[%d].x + 0.5)"
                                                                       : "#define %s (u_set.v[%d].xyz)", name, idx);
    return true;
}

// The pass's text with its settings' lines replaced (line numbers kept)
static char *with_settings(const char *body, int e) {
    size_t cap = strlen(body) * 2 + 1024, n = 0;
    char *out = malloc(cap);
    for (const char *p = body; *p;) {
        size_t l = strcspn(p, "\n");
        char line[600], rep[200];
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line ? l : sizeof line - 1), p);
        const char *use = e >= 0 && setting_line(line, e, rep, sizeof rep) ? rep : NULL;
        size_t ul = use ? strlen(use) : l;
        if (n + ul + 2 > cap) { cap = (n + ul + 2) * 2; out = realloc(out, cap); }
        memcpy(out + n, use ? use : p, ul);
        n += ul;
        p += l;
        if (*p == '\n') out[n++] = '\n', p++;
    }
    out[n] = 0;
    return out;
}

static void load_pass(int dir, const char *mod, const char *path, const char *file) {
    if (npass >= MAX_PASS) { LOG("[post] more than %d passes: %s left out\n", MAX_PASS, path); return; }
    char *raw = read_file(path);
    if (!raw) { LOG("[post] cannot read %s\n", path); return; }
    int e = effect_of(dir, mod, file, raw);
    char *body = with_settings(raw, e);
    free(raw);
    size_t n = sizeof PRELUDE + strlen(body) + sizeof EPILOGUE;
    char *src = malloc(n);
    snprintf(src, n, "%s%s%s", PRELUDE, body, EPILOGUE);
    free(body);
    Pass *p = &passes[npass];
    snprintf(p->name, sizeof p->name, "%s/%s", mod, file);
    p->effect = e;
    size_t nw = 0;
    uint32_t *spv = vk_glsl_to_spirv(src, STAGE_COMPUTE, p->name, &nw);
    free(src);
    if (!spv) { LOG("[post] %s did not compile (the errors above; line numbers are the file's): left out\n", p->name); return; }
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkResult res = m ? vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &p->pipe) : VK_ERROR_UNKNOWN;
    if (m) vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (res != VK_SUCCESS) { LOG("[post] %s: no pipeline (%d): left out\n", p->name, res); return; }
    LOG("[post] pass %d: %s\n", npass, p->name);
    npass++;
}

static void load_dir(int di) {
    const char *shaders = dirs[di];
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/post", shaders);
    DIR *d = opendir(dir);
    if (!d) return;
    char *files[256];
    int nf = 0;
    for (struct dirent *e; (e = readdir(d)) && nf < 256;) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '_' || e->d_name[0] == '.' || l < 6 || strcmp(e->d_name + l - 5, ".glsl")) continue;
        files[nf++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(files, nf, sizeof *files, by_name);
    // the mod's name: the folder under mods/ that holds shaders/
    const char *mod = shaders + strlen(mods_dir());
    while (*mod == '/' || *mod == '\\') mod++;
    char name[128];
    size_t k = strcspn(mod, "/\\");
    snprintf(name, sizeof name, "%.*s", (int)(k < sizeof name ? k : sizeof name - 1), mod);
    for (int i = 0; i < nf; i++) {
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        load_pass(di, name, path, files[i]);
        free(files[i]);
    }
}

// ------------------------------------------------------------------ settings.ini (beside post/)
static PostSetting *find_set(int e, const char *name) {
    for (int i = 0; i < nset; i++)
        if (sets[i].effect == e && !strcmp(sets[i].name, name)) return &sets[i];
    return NULL;
}

static void load_ini(int di) {
    char path[1200];
    snprintf(path, sizeof path, "%s/settings.ini", dirs[di]);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int e = -1;
    while (fgets(line, sizeof line, f)) {
        char *t = trim(line);
        size_t n = strlen(t);
        if (n && t[n - 1] == '\n') t[--n] = 0;
        t = trim(t);
        if (!*t || *t == '#' || *t == ';') continue;
        if (*t == '[') {
            char *end = strchr(t, ']');
            if (end) *end = 0;
            e = -1;
            for (int i = 0; i < neffect; i++)
                if (effects[i].dir == di && !strcmp(effects[i].name, t + 1)) e = i;
            continue;
        }
        char *eq = strchr(t, '=');
        if (e < 0 || !eq) continue;
        *eq = 0;
        char *key = trim(t), *v = trim(eq + 1);
        if (!strcmp(key, "on")) { effects[e].on = atoi(v) != 0; continue; }
        PostSetting *st = find_set(e, key);
        if (!st) continue;
        float x[3] = {0, 0, 0};
        int got = sscanf(v, "%f %f %f", &x[0], &x[1], &x[2]);
        if (st->type == POST_COLOR ? got == 3 : got >= 1) memcpy(st->val, x, sizeof x);
    }
    fclose(f);
    LOG("[post] settings from %s\n", path);
}

static void save_ini(int di) {
    char path[1200];
    snprintf(path, sizeof path, "%s/settings.ini", dirs[di]);
    FILE *f = fopen(path, "w");
    if (!f) { LOG("[post] cannot write %s\n", path); return; }
    fprintf(f, "# Written by the Shader mods window (F5). Only what differs from the passes' own values is kept;\n"
               "# delete a line to get that default back.\n");
    for (int e = 0; e < neffect; e++) {
        if (effects[e].dir != di) continue;
        bool header = false;
        if (effects[e].on != effects[e].def_on) fprintf(f, "\n[%s]\non = %d\n", effects[e].name, effects[e].on), header = true;
        for (int i = 0; i < nset; i++) {
            PostSetting *st = &sets[i];
            if (st->effect != e || !memcmp(st->val, st->def, sizeof st->val)) continue;
            if (!header) fprintf(f, "\n[%s]\n", effects[e].name), header = true;
            if (st->type == POST_COLOR) fprintf(f, "%s = %g %g %g\n", st->name, st->val[0], st->val[1], st->val[2]);
            else fprintf(f, "%s = %g\n", st->name, st->val[0]);
        }
    }
    fclose(f);
}

bool vk_post_available(void) { return g_post_on; }
void vk_post_lock(void) { pthread_mutex_lock(&set_lock); }
void vk_post_unlock(void) { pthread_mutex_unlock(&set_lock); }
int vk_post_effects(void) { return neffect; }
const char *vk_post_effect_name(int e) { return effects[e].name; }
const char *vk_post_effect_mod(int e) { return effects[e].mod; }
bool *vk_post_effect_on(int e) { return &effects[e].on; }
int vk_post_settings(PostSetting **out) { *out = sets; return nset; }
void vk_post_changed(void) {
    dirty_ns = plat_now_ns();
    atomic_store(&sets_dirty, true);
}

// The settings and effects' on / off, written once they have not changed for a second (render thread, at a present)
static void save_due(void) {
    if (!atomic_load(&sets_dirty) || plat_now_ns() - dirty_ns < 1000000000ull) return;
    atomic_store(&sets_dirty, false);
    pthread_mutex_lock(&set_lock);
    for (int d = 0; d < ndirs; d++) save_ini(d);
    pthread_mutex_unlock(&set_lock);
}

void vk_post_init(void) {
    ndirs = mods_shader_dirs(dirs, 64);
    if (!ndirs) return;
    logging = getenv("MHGU_POST_LOG") != NULL;
    VkDescriptorSetLayoutBinding b[7];
    static const VkDescriptorType types[7] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    for (uint32_t i = 0; i < 7; i++) b[i] = (VkDescriptorSetLayoutBinding){i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 7,
                                          .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &layout));
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &mip_sampler));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &light_buf, &light_mem, NULL));
    vkCmdFillBuffer(vk_upload_cmd(), light_buf, 0, 64, 0);
    {  // the light pass: the prelude with u_light writable, and LIGHT_MAIN
        static const char head[] = "#version 450\n#define LIGHT_PASS 1\n";
        size_t n = sizeof head + sizeof PRELUDE + sizeof LIGHT_MAIN;
        char *src = malloc(n);
        snprintf(src, n, "%s%s%s", head, PRELUDE + strlen("#version 450\n"), LIGHT_MAIN);
        size_t nw = 0;
        uint32_t *spv = vk_glsl_to_spirv(src, STAGE_COMPUTE, "post_light", &nw);
        free(src);
        VkShaderModule m = spv ? vk_module(spv, nw) : VK_NULL_HANDLE;
        free(spv);
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                          .layout = layout};
        if (!m || vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &light_pipe) != VK_SUCCESS) {
            light_pipe = VK_NULL_HANDLE;
            LOG("[post] the light pass did not build: u_light stays empty\n");
        }
        if (m) vkDestroyShaderModule(g_vk.dev, m, NULL);
    }
    pthread_mutex_lock(&set_lock);
    for (int i = 0; i < ndirs; i++) load_dir(i);
    for (int i = 0; i < ndirs; i++) load_ini(i);
    pthread_mutex_unlock(&set_lock);
    g_post_on = true;  // (even with none compiled: Shift+F6 may load them once fixed)
    t0 = plat_now_ns();
    LOG("[post] %d shader mod pass(es) from %d mod(s) (F6: off / on, Shift+F6: read them again)\n", npass, ndirs);
}

bool vk_post_active(void) { return atomic_load(&enabled); }

void vk_post_toggle(void) {
    bool on = !atomic_load(&enabled);
    atomic_store(&enabled, on);
    LOG("[post] F6: shader mods %s\n", on ? "on" : "off");
}

void vk_post_reload(void) { atomic_store(&reload_asked, true); }

// Shift+F6: the passes compiled again from the files (render thread, before this frame's passes are recorded)
static void reload(void) {
    vkDeviceWaitIdle(g_vk.dev);  // (no earlier frame's dispatch may still use the old pipelines)
    for (int i = 0; i < npass; i++) vkDestroyPipeline(g_vk.dev, passes[i].pipe, NULL);
    npass = 0;
    pthread_mutex_lock(&set_lock);
    static Effect old_e[MAX_EFFECT];  // (what the window set is kept: matched by effect and setting name)
    static PostSetting old_s[MAX_SET];
    int one = neffect, ons = nset;
    memcpy(old_e, effects, sizeof effects);
    memcpy(old_s, sets, sizeof sets);
    neffect = nset = 0;
    for (int i = 0; i < ndirs; i++) load_dir(i);
    for (int e = 0; e < neffect; e++)
        for (int o = 0; o < one; o++)
            if (old_e[o].dir == effects[e].dir && !strcmp(old_e[o].name, effects[e].name)) {
                effects[e].on = old_e[o].on;
                for (int k = 0; k < ons; k++) {
                    PostSetting *st = old_s[k].effect == o ? find_set(e, old_s[k].name) : NULL;
                    if (st && st->type == old_s[k].type && memcmp(old_s[k].val, old_s[k].def, sizeof st->val))
                        memcpy(st->val, old_s[k].val, sizeof st->val);
                }
            }
    pthread_mutex_unlock(&set_lock);
    LOG("[post] Shift+F6: %d pass(es) read again\n", npass);
}

// ================================================================== the frame
static NvnTexture *g_h;                  // the full-screen image (learnt at the presents, as fsr.c does)
static NvnTexture *last_target, *last_src;
static struct { NvnTexture *t; uint32_t n; } depths[4];
static bool frame_done;                  // the passes ran (or were passed over) this frame
static uint32_t n_run, n_skip;
static const char *why_skip;
static float last_cam[3];  // (MHGU_POST_LOG) near, far, fovy of the last frame the passes ran on

static bool is_rt(NvnTexture *t) { return t && t->backend && ((TexBackend *)t->backend)->gpu_owned; }
static bool presented_texture(NvnTexture *t) { return t && t->backend && ((TexBackend *)t->backend)->presented; }

static uint32_t mip_count(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while ((w | h) >> n) n++;
    return n;
}

static bool own_image(TexBackend *b, uint32_t w, uint32_t h, bool mips) {
    uint32_t levels = mips ? mip_count(w, h) : 1;
    if (b->image && b->width == w && b->height == h && b->levels == levels) return true;
    if (b->image) { vk_views_forget(b); vk_defer_image(b->image, b->mem); }
    memset(b, 0, sizeof *b);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) != VK_SUCCESS) { b->image = VK_NULL_HANDLE; return false; }
    b->format = VK_FORMAT_R16G16B16A16_SFLOAT, b->base_type = VK_IMAGE_VIEW_TYPE_2D, b->width = w, b->height = h;
    b->depth = b->layers = b->samples = 1, b->levels = levels;
    b->aspect = VK_IMAGE_ASPECT_COLOR_BIT, b->gpu_owned = true;
    vk_image_init_layout(vk_cmd(), b->image, b->aspect);
    return true;
}

// img[0]'s mipmaps from its level 0
static void make_mips(VkCommandBuffer cb, TexBackend *b) {
    for (uint32_t l = 1; l < b->levels; l++) {
        int32_t sw = (int32_t)(b->width >> (l - 1)), sh = (int32_t)(b->height >> (l - 1));
        int32_t dw = (int32_t)(b->width >> l), dh = (int32_t)(b->height >> l);
        VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, .srcOffsets = {{0, 0, 0}, {sw ? sw : 1, sh ? sh : 1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, .dstOffsets = {{0, 0, 0}, {dw ? dw : 1, dh ? dh : 1, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
}

static void blit(VkCommandBuffer cb, TexBackend *s, TexBackend *d) {
    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)s->width, (int32_t)s->height, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)d->width, (int32_t)d->height, 1}}};
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_NEAREST);
}

// row-vector 4x4 inverse (Gauss-Jordan with partial pivoting, as fsr.c's)
static bool mat_inv(const float *m, float *out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)a[r][c + 4];
    return true;
}

static VkSpan set_span;  // this frame's copy of the settings' values (u_set)

static VkDescriptorSet make_set(TexBackend *scene, TexBackend *depth, TexBackend *orig, TexBackend *out, VkSpan ubo) {
    static const VkComponentMapping id = {0};
    VkDescriptorSet set = vk_alloc_set(set_layout);
    VkDescriptorImageInfo ii[4] = {
        {vk_default_sampler(true), vk_image_view(scene, VK_IMAGE_VIEW_TYPE_2D, scene->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_GENERAL},
        {vk_default_sampler(false), vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_GENERAL},
        {mip_sampler, vk_image_view(orig, VK_IMAGE_VIEW_TYPE_2D, orig->format, 0, orig->levels, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, vk_image_view(out, VK_IMAGE_VIEW_TYPE_2D, out->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_GENERAL}};
    for (int i = 0; i < 4; i++) if (!ii[i].imageView) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[3] = {{ubo.buf, ubo.off, sizeof(FrameU)}, {light_buf, 0, 64}, {set_span.buf, set_span.off, MAX_SET * 16}};
    VkWriteDescriptorSet w[7];
    for (uint32_t i = 0; i < 7; i++) {
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
                                      .descriptorCount = 1,
                                      .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                        : i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                        : i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        if (i < 4) w[i].pImageInfo = &ii[i];
        else w[i].pBufferInfo = &bi[i - 4];
    }
    vkUpdateDescriptorSets(g_vk.dev, 7, w, 0, NULL);
    return set;
}

static void skip(const char *why) { n_skip++, why_skip = why; }

static void run(void) {
    frame_done = true;
    if (atomic_exchange(&reload_asked, false)) { vk_end_pass(); reload(); }
    if (!atomic_load(&enabled)) { skip("turned off (F6)"); return; }
    if (!npass) { skip("no pass compiled"); return; }
    int best = -1;
    for (int i = 0; i < 4; i++) if (depths[i].t && (best < 0 || depths[i].n > depths[best].n)) best = i;
    if (best < 0 || depths[best].n < 16) { skip("no 3D scene this frame"); return; }
    CamRecord cam;
    if (!camjit_latest(&cam) || cam.present + 1 < g_present_count) { skip("the main camera was not computed (event camera, menu)"); return; }
    vk_end_pass();
    TexBackend *hb = vk_tex(g_h, true), *db = vk_tex(depths[best].t, true);
    vk_ms_resolve(hb);
    vk_ms_resolve(db);
    uint32_t w = hb->width, h = hb->height;
    if (db->width != w || db->height != h) { skip("the depth target is not the picture's size"); return; }
    if (!own_image(&img[0], w, h, true) || !own_image(&img[1], w, h, false) || !own_image(&img[2], w, h, false)) { skip("out of GPU memory"); return; }
    VkSpan ubo = vk_transient(sizeof(FrameU), g_vk.props.limits.minUniformBufferOffsetAlignment);
    FrameU *f = ubo.ptr;
    memcpy(f->proj, cam.proj, 64);
    memcpy(f->view, cam.view, 64);
    if (!mat_inv(cam.proj, f->inv_proj) || !mat_inv(cam.view, f->inv_view)) { skip("the camera's matrices cannot be inverted"); return; }
    f->size[0] = (float)w, f->size[1] = (float)h, f->size[2] = 1.0f / w, f->size[3] = 1.0f / h;
    f->info[0] = (float)((plat_now_ns() - t0) / 1e9), f->info[1] = (float)g_present_count, f->info[2] = (float)npass, f->info[3] = 0;
    f->cam[0] = cam.near, f->cam[1] = cam.far, f->cam[2] = cam.fovy, f->cam[3] = 0;
    last_cam[0] = cam.near, last_cam[1] = cam.far, last_cam[2] = cam.fovy;
    f->eye[0] = f->inv_view[12], f->eye[1] = f->inv_view[13], f->eye[2] = f->inv_view[14], f->eye[3] = 1.0f;
    set_span = vk_transient(MAX_SET * 16, g_vk.props.limits.minUniformBufferOffsetAlignment);
    bool pass_on[MAX_PASS];
    {
        float (*v)[4] = set_span.ptr;
        memset(v, 0, MAX_SET * 16);
        pthread_mutex_lock(&set_lock);
        for (int i = 0; i < nset; i++) memcpy(v[i], sets[i].val, 12);
        for (int i = 0; i < npass; i++) pass_on[i] = passes[i].effect < 0 || effects[passes[i].effect].on;
        pthread_mutex_unlock(&set_lock);
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    blit(cb, hb, &img[0]);
    vk_full_barrier(cb);
    make_mips(cb, &img[0]);
    if (light_pipe) {  // the sun / moon and the sky (u_light); it writes no image: img[1] is bound as a stand-in
        VkDescriptorSet set = make_set(&img[0], db, &img[0], &img[1], ubo);
        if (set) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, light_pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
            vkCmdDispatch(cb, 1, 1, 1);
            vk_full_barrier(cb);
        }
    }
    int src = 0;
    for (int i = 0; i < npass; i++) {
        if (!pass_on[i]) continue;
        int dst = src == 1 ? 2 : 1;
        VkDescriptorSet set = make_set(&img[src], db, &img[0], &img[dst], ubo);
        if (!set) continue;
        float push[4] = {(float)i, 0, 0, 0};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, passes[i].pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        vk_full_barrier(cb);
        src = dst;
    }
    if (src) blit(cb, &img[src], hb);
    vk_full_barrier(cb);
    n_run++;
}

void vk_post_draw(const FsrDraw *d) {
    last_target = d->color0;
    last_src = d->ntex ? d->tex[0] : NULL;
    if (!g_h) return;
    if (d->depth && d->depth_write && d->depth->width == g_h->width && d->depth->height == g_h->height) {
        int i = 0;
        while (i < 4 && depths[i].t && depths[i].t != d->depth) i++;
        if (i < 4) depths[i].t = d->depth, depths[i].n++;
    }
    if (frame_done || !d->color0) return;
    if (d->color0 == g_h) {
        bool reads_rt = false;
        for (int i = 0; i < d->ntex; i++) reads_rt |= d->tex[i] != g_h && is_rt(d->tex[i]);
        if (d->count <= 6 && reads_rt) return;  // (one of the game's own post passes: bloom, FXAA...)
        run();
        return;
    }
    if (presented_texture(d->color0)) {
        bool reads_h = false;
        for (int i = 0; i < d->ntex; i++) reads_h |= d->tex[i] == g_h;
        if (reads_h) run();
    }
}

void vk_post_present(NvnTexture *t) {
    if (last_target == t && last_src && last_src != t && last_src->backend &&
        !(((TexBackend *)last_src->backend)->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        if (g_h != last_src) LOG("[post] full-screen image: %p %ux%u\n", (void *)last_src, last_src->width, last_src->height);
        g_h = last_src;
    }
    save_due();
    if (logging) {
        static uint64_t log_t;
        uint64_t now = plat_now_ns();
        if (!log_t) log_t = now;
        if (now - log_t >= 1000000000ull) {
            LOG("[post] 1s: passes ran %u frames, passed over %u%s%s | camera near %g far %g fovy %.1f deg\n", n_run, n_skip,
                n_skip ? ", last: " : "", n_skip ? why_skip : "", last_cam[0], last_cam[1], last_cam[2] * 57.29578f);
            n_run = n_skip = 0;
            log_t = now;
        }
    }
    memset(depths, 0, sizeof depths);
    frame_done = false;
    last_target = last_src = NULL;
}
