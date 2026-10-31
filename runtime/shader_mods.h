// Shader mods' effects and settings, for the Shader mods window (runtime/overlay.cpp, F5): the state lives in
// runtime/nvn/vulkan/post.c. C and C++; no Vulkan here.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A number a pass marked as a setting: `const float NAME = 1.0;  // [0, 2] Label: help` ([color] for a vec3)
enum { POST_FLOAT, POST_INT, POST_COLOR };
typedef struct {
    char name[48];             // the constant's name in the pass
    char label[64], help[192]; // what the window shows (the comment after the range: before ':' and after it)
    int effect, type;          // the effect it belongs to (vk_post_effect_*), POST_*
    float lo, hi;              // its range
    float def[3], val[3];      // the pass's own value, the value used (POST_COLOR: rgb; else [0])
} PostSetting;

bool vk_post_available(void);  // shader mods with passes were found at start-up
bool vk_post_active(void);     // their passes run (F6)
void vk_post_toggle(void);     // F6
void vk_post_reload(void);     // Shift+F6: the files read again

// Under the lock (the render thread reads the values each frame, Shift+F6 rebuilds the lists):
void vk_post_lock(void);
void vk_post_unlock(void);
int vk_post_effects(void);                 // effects: the passes that share a name (the first line of a pass:
const char *vk_post_effect_name(int e);    // "// Name: what it does"), in the order they run
const char *vk_post_effect_mod(int e);     // the mod it comes from
bool *vk_post_effect_on(int e);
int vk_post_settings(PostSetting **out);   // every setting, effect by effect
void vk_post_changed(void);                // after a change: saved to the mod's shaders/settings.ini within a second

#ifdef __cplusplus
}
#endif
